#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

#ifndef VM_USE_PTHREAD
#define VM_USE_PTHREAD 0
#endif
#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__)) && !VM_USE_PTHREAD
#include <sched.h>
#define VM_LINUX_THREADS 1
#else
#define VM_LINUX_THREADS 0
#endif

#define PAGE_SIZE 256
#define PAGE_COUNT 256
#define TLB_SIZE 16
#ifndef FRAME_COUNT
#define FRAME_COUNT 128
#endif
#ifndef USE_TLB
#define USE_TLB 1
#endif
#ifndef THREADED_TLB
#define THREADED_TLB 1
#endif

#if FRAME_COUNT != 128 && FRAME_COUNT != 256
#error "FRAME_COUNT deve ser 128 ou 256"
#endif
#if (USE_TLB != 0 && USE_TLB != 1) || (THREADED_TLB != 0 && THREADED_TLB != 1)
#error "USE_TLB e THREADED_TLB devem ser 0 ou 1"
#endif

typedef enum { FIFO, LRU } Policy;

typedef struct {
    int page;
    int frame;
    size_t inserted_at;
    size_t used_at;
} TlbEntry;

typedef struct {
    int page_table[PAGE_COUNT];
    int frame_page[FRAME_COUNT];
    unsigned char memory[FRAME_COUNT][PAGE_SIZE];
    size_t loaded_at[FRAME_COUNT];
    size_t used_at[FRAME_COUNT];
    TlbEntry tlb[TLB_SIZE];
    size_t loaded_frames;
    size_t translated;
    size_t faults;
    size_t hits;
    Policy page_policy;
    Policy tlb_policy;
} Vm;

typedef struct {
    const TlbEntry *entry;
    int page;
    int matched;
#if VM_LINUX_THREADS
    atomic_int finished;
    atomic_int tid;
#endif
} SearchTask;

static int parse_policy(const char *name, Policy *policy)
{
    if (strcmp(name, "fifo") == 0) {
        *policy = FIFO;
        return 0;
    }
    if (strcmp(name, "lru") == 0) {
        *policy = LRU;
        return 0;
    }
    return -1;
}

/* Confere o backing com leituras comuns, sem consultar metadados do arquivo. */
static int validate_backing(FILE *file)
{
    unsigned char buffer[1024];
    size_t total = 0;
    size_t amount;
    while ((amount = fread(buffer, 1, sizeof(buffer), file)) != 0) {
        total += amount;
        if (total > PAGE_COUNT * PAGE_SIZE)
            break;
    }
    if (ferror(file)) {
        fprintf(stderr, "Erro ao ler BACKING_STORE.bin: %s.\n", strerror(errno));
        return -1;
    }
    if (total != PAGE_COUNT * PAGE_SIZE) {
        fprintf(stderr, "Erro: BACKING_STORE.bin deve conter exatamente 65536 bytes.\n");
        return -1;
    }
    return 0;
}

/* A entrada inteira e validada antes de abrir a saida. */
static int read_addresses(const char *name, uint32_t **addresses, size_t *count)
{
    FILE *file = fopen(name, "rb");
    uint32_t *items = NULL;
    char *line = NULL;
    char input_buffer[4096];
    size_t line_capacity = 0, capacity = 0, line_number = 0;
    int status = -1;
    ssize_t length;

    *addresses = NULL;
    *count = 0;
    if (!file) {
        fprintf(stderr, "Erro ao abrir os enderecos '%s': %s.\n", name, strerror(errno));
        return -1;
    }

    if (setvbuf(file, input_buffer, _IOFBF, sizeof(input_buffer)) != 0) {
        fprintf(stderr, "Erro ao configurar a leitura dos enderecos.\n");
        goto done;
    }

    while ((length = getline(&line, &line_capacity, file)) >= 0) {
        size_t pos = 0;
        uint32_t value = 0;
        int has_digit = 0;
        ++line_number;
        if (memchr(line, '\0', (size_t)length) != NULL)
            goto invalid;
        while (pos < (size_t)length && isspace((unsigned char)line[pos]))
            ++pos;
        while (pos < (size_t)length && line[pos] >= '0' && line[pos] <= '9') {
            unsigned int digit = (unsigned int)(line[pos] - '0');
            if (value > (UINT32_MAX - digit) / 10U)
                goto invalid;
            value = value * 10U + digit;
            has_digit = 1;
            ++pos;
        }
        while (pos < (size_t)length && isspace((unsigned char)line[pos]))
            ++pos;
        if (!has_digit || pos != (size_t)length)
            goto invalid;

        if (*count == capacity) {
            size_t next = capacity ? capacity * 2 : 1024;
            uint32_t *grown;
            if (next < capacity || next > SIZE_MAX / sizeof(*items)) {
                fprintf(stderr, "Erro: arquivo de enderecos grande demais.\n");
                goto done;
            }
            grown = realloc(items, next * sizeof(*items));
            if (!grown) {
                fprintf(stderr, "Erro: memoria insuficiente para os enderecos.\n");
                goto done;
            }
            items = grown;
            capacity = next;
        }
        items[(*count)++] = value;
    }
    if (ferror(file) || !feof(file)) {
        fprintf(stderr, "Erro durante a leitura de '%s'.\n", name);
        goto done;
    }
    if (*count == 0) {
        fprintf(stderr, "Erro: o arquivo de enderecos esta vazio.\n");
        goto done;
    }
    status = 0;
    goto done;

invalid:
    fprintf(stderr, "Erro: formato invalido na linha %zu de '%s'; "
                    "use um inteiro decimal de 0 a %" PRIu32 " por linha.\n",
            line_number, name, UINT32_MAX);
done:
    free(line);
    if (fclose(file) != 0) {
        fprintf(stderr, "Erro ao fechar os enderecos: %s.\n", strerror(errno));
        status = -1;
    }
    if (status == 0)
        *addresses = items;
    else {
        free(items);
        *count = 0;
    }
    return status;
}

static void initialize(Vm *vm, Policy page_policy, Policy tlb_policy)
{
    memset(vm, 0, sizeof(*vm));
    for (int i = 0; i < PAGE_COUNT; ++i)
        vm->page_table[i] = -1;
    for (int i = 0; i < FRAME_COUNT; ++i)
        vm->frame_page[i] = -1;
    for (int i = 0; i < TLB_SIZE; ++i) {
        vm->tlb[i].page = -1;
        vm->tlb[i].frame = -1;
    }
    vm->page_policy = page_policy;
    vm->tlb_policy = tlb_policy;
}

#if VM_LINUX_THREADS
/* Os workers fazem apenas a comparacao: nenhuma chamada de libc ou uso de TLS. */
static int search_entry_linux(void *argument)
{
    SearchTask *task = argument;
    task->matched = task->entry->page == task->page;
    atomic_store_explicit(&task->finished, 1, memory_order_release);
    return 0;
}
#else
static void *search_entry(void *argument)
{
    SearchTask *task = argument;
    task->matched = task->entry->page == task->page;
    return NULL;
}
#endif

static int search_tlb(const Vm *vm, int page, int *slot)
{
    *slot = -1;
    if (!USE_TLB)
        return 0;
    if (!THREADED_TLB) {
        for (int i = 0; i < TLB_SIZE; ++i) {
            if (vm->tlb[i].page == page) {
                *slot = i;
                break;
            }
        }
        return 0;
    }

#if VM_LINUX_THREADS
    enum { THREAD_STACK_SIZE = 64 * 1024 };
    _Alignas(16) static unsigned char stacks[TLB_SIZE][THREAD_STACK_SIZE];
    _Static_assert(ATOMIC_INT_LOCK_FREE == 2, "O TID precisa de acesso atomico sem locks");
    _Static_assert(sizeof(atomic_int) == sizeof(int), "TID incompativel com clone");
    SearchTask tasks[TLB_SIZE];
    int created = 0, status = 0;
    const int flags = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND |
                      CLONE_THREAD | CLONE_SYSVSEM | CLONE_PARENT_SETTID |
                      CLONE_CHILD_CLEARTID;

    for (int i = 0; i < TLB_SIZE; ++i) {
        tasks[i].entry = &vm->tlb[i];
        tasks[i].page = page;
        tasks[i].matched = 0;
        atomic_init(&tasks[i].finished, 0);
        atomic_init(&tasks[i].tid, -1);
        if (clone(search_entry_linux, stacks[i] + THREAD_STACK_SIZE, flags,
                  &tasks[i], (int *)&tasks[i].tid, NULL, (int *)&tasks[i].tid) == -1) {
            fprintf(stderr, "Erro ao criar a thread da TLB %d: %s.\n", i, strerror(errno));
            status = -1;
            break;
        }
        ++created;
    }
    for (int i = 0; i < created; ++i) {
        /*
         * finished publica o resultado em C11. O kernel zera tid somente na
         * saida da thread; assim a pilha nao e reutilizada antes do termino.
         */
        while (!atomic_load_explicit(&tasks[i].finished, memory_order_acquire) ||
               atomic_load_explicit(&tasks[i].tid, memory_order_acquire) != 0) {
            /* Deixa os workers executarem mesmo com apenas uma CPU disponivel. */
            sched_yield();
        }
        if (tasks[i].matched)
            *slot = i;
    }
    return status;
#else
    pthread_t threads[TLB_SIZE];
    SearchTask tasks[TLB_SIZE];
    int created = 0, status = 0;
    for (int i = 0; i < TLB_SIZE; ++i) {
        int error;
        tasks[i].entry = &vm->tlb[i];
        tasks[i].page = page;
        tasks[i].matched = 0;
        error = pthread_create(&threads[i], NULL, search_entry, &tasks[i]);
        if (error != 0) {
            fprintf(stderr, "Erro ao criar a thread da TLB %d: %s.\n", i, strerror(error));
            status = -1;
            break;
        }
        ++created;
    }
    /* Nenhuma entrada da TLB e modificada antes de todas as buscas terminarem. */
    for (int i = 0; i < created; ++i) {
        int error = pthread_join(threads[i], NULL);
        if (error != 0) {
            fprintf(stderr, "Erro ao aguardar a thread da TLB %d: %s.\n", i, strerror(error));
            /* Nao voltar a main com threads usando as variaveis desta pilha. */
            exit(EXIT_FAILURE);
        }
        if (tasks[i].matched)
            *slot = i;
    }
    return status;
#endif
}

static void invalidate_tlb(Vm *vm, int page)
{
    for (int i = 0; i < TLB_SIZE; ++i) {
        if (vm->tlb[i].page == page) {
            vm->tlb[i].page = -1;
            vm->tlb[i].frame = -1;
        }
    }
}

static int insert_tlb(Vm *vm, int page, int frame, size_t now)
{
    int victim = 0;
    for (int i = 0; i < TLB_SIZE; ++i) {
        if (vm->tlb[i].page == -1) {
            victim = i;
            goto insert;
        }
        size_t age = vm->tlb_policy == FIFO ? vm->tlb[i].inserted_at : vm->tlb[i].used_at;
        size_t oldest = vm->tlb_policy == FIFO ? vm->tlb[victim].inserted_at : vm->tlb[victim].used_at;
        if (age < oldest)
            victim = i;
    }
insert:
    vm->tlb[victim].page = page;
    vm->tlb[victim].frame = frame;
    vm->tlb[victim].inserted_at = now;
    vm->tlb[victim].used_at = now;
    return victim;
}

static int load_page(Vm *vm, FILE *backing, int page, size_t now)
{
    int frame = 0;
    unsigned char contents[PAGE_SIZE];
    if (fseek(backing, (long)page * PAGE_SIZE, SEEK_SET) != 0 ||
        fread(contents, 1, PAGE_SIZE, backing) != PAGE_SIZE) {
        fprintf(stderr, "Erro ao ler a pagina %d de BACKING_STORE.bin.\n", page);
        return -1;
    }
    if (vm->loaded_frames < FRAME_COUNT) {
        frame = (int)vm->loaded_frames++;
    } else {
        for (int i = 1; i < FRAME_COUNT; ++i) {
            size_t age = vm->page_policy == FIFO ? vm->loaded_at[i] : vm->used_at[i];
            size_t oldest = vm->page_policy == FIFO ? vm->loaded_at[frame] : vm->used_at[frame];
            if (age < oldest)
                frame = i;
        }
        int old_page = vm->frame_page[frame];
        vm->page_table[old_page] = -1;
        invalidate_tlb(vm, old_page);
    }
    memcpy(vm->memory[frame], contents, PAGE_SIZE);
    vm->frame_page[frame] = page;
    vm->page_table[page] = frame;
    vm->loaded_at[frame] = now;
    ++vm->faults;
    return frame;
}

static int translate(Vm *vm, FILE *backing, FILE *output, uint32_t address)
{
    uint32_t logical = address & UINT32_C(0xffff);
    int page = (int)(logical >> 8);
    int offset = (int)(logical & UINT32_C(0xff));
    int slot, frame;
    size_t now = vm->translated + 1;
    if (search_tlb(vm, page, &slot) != 0)
        return -1;
    if (slot >= 0) {
        frame = vm->tlb[slot].frame;
        vm->tlb[slot].used_at = now;
        ++vm->hits;
    } else {
        frame = vm->page_table[page];
        if (frame < 0) {
            frame = load_page(vm, backing, page, now);
            if (frame < 0)
                return -1;
        }
        if (USE_TLB)
            slot = insert_tlb(vm, page, frame, now);
    }
    /* LRU de paginas considera TODOS os acessos, incluindo hits na TLB. */
    vm->used_at[frame] = now;
    int byte = vm->memory[frame][offset];
    int value = byte < 128 ? byte : byte - 256;
    if (fprintf(output, "Virtual address: %" PRIu32 " TLB: %d Physical address: %d Value: %d\n",
                address, slot, frame * PAGE_SIZE + offset, value) < 0) {
        fprintf(stderr, "Erro ao escrever as traducoes.\n");
        return -1;
    }
    ++vm->translated;
    return 0;
}

int main(int argc, char **argv)
{
    Vm vm;
    Policy page_policy, tlb_policy;
    uint32_t *addresses = NULL;
    size_t count = 0;
    FILE *backing = NULL, *output = NULL;
    int status = EXIT_FAILURE;
    char backing_buffer[4096], output_buffer[4096];

    if (setvbuf(stdout, NULL, _IONBF, 0) != 0) {
        fprintf(stderr, "Erro ao configurar a saida do terminal.\n");
        return EXIT_FAILURE;
    }

    if (argc != 4) {
        fprintf(stderr, "Erro: informe exatamente 3 argumentos.\n"
                        "Uso: %s <arquivo_de_enderecos> <fifo|lru> <fifo|lru>\n", argv[0]);
        return EXIT_FAILURE;
    }
    if (parse_policy(argv[2], &page_policy) != 0 || parse_policy(argv[3], &tlb_policy) != 0) {
        fprintf(stderr, "Erro: as politicas de paginas e de TLB devem ser 'fifo' ou 'lru'.\n");
        return EXIT_FAILURE;
    }
    if (read_addresses(argv[1], &addresses, &count) != 0)
        return EXIT_FAILURE;
    backing = fopen("BACKING_STORE.bin", "rb");
    if (!backing) {
        fprintf(stderr, "Erro ao abrir BACKING_STORE.bin: %s.\n", strerror(errno));
        goto done;
    }
    if (setvbuf(backing, backing_buffer, _IOFBF, sizeof(backing_buffer)) != 0) {
        fprintf(stderr, "Erro ao configurar a leitura de BACKING_STORE.bin.\n");
        goto done;
    }
    if (validate_backing(backing) != 0)
        goto done;
    output = fopen("correct.txt", "wb");
    if (!output) {
        fprintf(stderr, "Erro ao criar correct.txt: %s.\n", strerror(errno));
        goto done;
    }
    if (setvbuf(output, output_buffer, _IOFBF, sizeof(output_buffer)) != 0) {
        fprintf(stderr, "Erro ao configurar a escrita de correct.txt.\n");
        goto done;
    }
    initialize(&vm, page_policy, tlb_policy);
    for (size_t i = 0; i < count; ++i) {
        if (translate(&vm, backing, output, addresses[i]) != 0)
            goto done;
    }
    if (fprintf(output,
                "Number of Translated Addresses = %zu\n"
                "Page Faults = %zu\n"
                "Page Fault Rate = %.3f\n"
                "TLB Hits = %zu\n"
                "TLB Hit Rate = %.3f\n",
                vm.translated, vm.faults, (double)vm.faults / (double)vm.translated,
                vm.hits, (double)vm.hits / (double)vm.translated) < 0) {
        fprintf(stderr, "Erro ao escrever as estatisticas.\n");
        goto done;
    }
    if (fclose(output) != 0) {
        output = NULL;
        fprintf(stderr, "Erro ao fechar a saida: %s.\n", strerror(errno));
        goto done;
    }
    output = NULL;
    if (fclose(backing) != 0) {
        backing = NULL;
        fprintf(stderr, "Erro ao fechar BACKING_STORE.bin: %s.\n", strerror(errno));
        goto done;
    }
    backing = NULL;
    printf("Saida salva em correct.txt (%zu enderecos).\n", vm.translated);
    status = EXIT_SUCCESS;
done:
    if (output && fclose(output) != 0)
        fprintf(stderr, "Erro ao fechar a saida incompleta.\n");
    if (backing && fclose(backing) != 0)
        fprintf(stderr, "Erro ao fechar BACKING_STORE.bin.\n");
    free(addresses);
    return status;
}
