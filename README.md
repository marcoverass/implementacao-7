# Implementação 6 - Memória virtual

Identificação: Mavss.

O programa traduz endereços lógicos, consulta uma TLB de 16 entradas e carrega
páginas de 256 bytes sob demanda. A configuração padrão tem 128 frames e faz
a busca na TLB com 16 threads, uma para cada entrada.

## Compilação e execução

Requisitos: Linux, Unix ou macOS, compilador C com POSIX threads e `make`.
Os testes automatizados também usam Python 3. Não há bibliotecas Python externas.

Execute `make` na raiz do repositório, onde estão `Makefile` e `vm.c`.
O executável `vm` será criado nessa mesma pasta.

Na raiz do repositório, coloque o `BACKING_STORE.bin` fornecido na atividade.
O arquivo precisa ter exatamente 65.536 bytes. O arquivo de endereços pode estar
em outro diretório; informe seu caminho no primeiro argumento.

```sh
make clean
make
./vm addresses.txt lru fifo
```

O segundo argumento escolhe a política de páginas; o terceiro, a da TLB.
São aceitas apenas as palavras `fifo` e `lru`, em minúsculas.

```sh
./vm addresses.txt fifo fifo
./vm addresses.txt fifo lru
./vm addresses.txt lru fifo
./vm addresses.txt lru lru
```

Cada execução bem-sucedida salva `correct.txt` no diretório atual. A coluna
`TLB` mostra o índice da entrada consultada em um hit ou preenchida em um miss.
O rodapé informa a quantidade de traduções, faltas de página e acertos da TLB,
com taxas entre 0 e 1 e três casas decimais.

Antes de executar, preserve o `correct.txt` do professor com outro nome,
por exemplo `correct_referencia.txt`, para ele não ser substituído pela saída.

## Testes

Deixe `BACKING_STORE.bin`, `addresses.txt` e `correct_referencia.txt` juntos
no diretório informado em `DATA_DIR`. O último deve ser uma cópia do
`correct.txt` fornecido pelo professor.

```sh
make test
```

Se os dados estiverem no diretório vizinho `dados`, use:

```sh
make test DATA_DIR=../dados
make etapas DATA_DIR=../dados
```

`make test` verifica as quatro combinações de políticas, compara LRU/FIFO com
o arquivo fornecido e confere traduções com um modelo independente em Python.
Também verifica extremos de endereço, hits, substituição, invalidação da TLB,
formatação CRLF, ausência da última quebra de linha, argumentos inválidos,
arquivo sem permissão, entrada vazia, linhas malformadas, backing incorreto e
falha ao salvar a saída. O teste de permissão informa `SKIP` quando o usuário
tem privilégio suficiente para ler um arquivo com modo zero.

`make etapas` compila e valida todas as configurações da rubrica. Ao terminar,
restaura o executável padrão. As opções também podem ser usadas isoladamente:

```sh
make FRAME_COUNT=256 USE_TLB=0 THREADED_TLB=0
make FRAME_COUNT=128 USE_TLB=0 THREADED_TLB=0
make FRAME_COUNT=128 USE_TLB=1 THREADED_TLB=0
make
```

A interface de execução continua sendo a mesma em todas as etapas. Sem TLB,
a coluna `TLB` recebe `-1` e o número de hits é zero. O Makefile recompila `vm`
a cada chamada para que uma mudança de configuração seja aplicada.

Resultados com os 1.000 endereços fornecidos, na configuração padrão:

| Páginas | TLB | Faltas de página | Acertos da TLB |
| --- | --- | ---: | ---: |
| FIFO | FIFO | 538 | 54 |
| FIFO | LRU | 538 | 55 |
| LRU | FIFO | 539 | 54 |
| LRU | LRU | 539 | 55 |

Com 256 frames e sem TLB, são 244 faltas de página. Com 128 frames sem TLB,
são 538 para FIFO e 539 para LRU. As versões sequencial e paralela da TLB
produzem os mesmos resultados.

## Decisões da implementação

- As páginas têm 256 bytes. Os 16 bits inferiores do inteiro de entrada são
  divididos em página e deslocamento. A saída preserva o inteiro original.
- Cada linha deve conter um inteiro decimal sem sinal de 0 a 4.294.967.295.
  Espaços ao redor do número e linhas CRLF são aceitos; linhas vazias, números
  negativos, sinais `+`, texto e mais de um número por linha são recusados.
- Os frames são ocupados na ordem 0 a 127. FIFO usa a ordem de carregamento;
  LRU usa o último acesso, inclusive quando a tradução veio da TLB.
- Ao retirar uma página, o programa invalida sua tradução na tabela e na TLB.
  Entradas livres da TLB são usadas pelo menor índice. Quando todas estão
  ocupadas, a vítima é a inserção mais antiga em FIFO ou o último uso mais
  antigo em LRU. Um hit não muda a ordem de inserção do FIFO.
- Cada busca cria 16 threads reais, uma por entrada da TLB. Em Linux x86-64
  e AArch64, elas usam `clone()` com `CLONE_THREAD`, sem depender de `clone3()`.
  Cada worker faz somente a comparação e publica o resultado por uma variável
  atômica. A thread principal espera o resultado e o término indicado pelo kernel
  antes de atualizar a TLB ou reutilizar a pilha. Durante a espera, `sched_yield()`
  permite que os workers executem mesmo com apenas uma CPU. Os workers não chamam funções
  da libc nem usam dados locais de thread. Nos demais sistemas, a implementação
  usa `pthread_create` e `pthread_join`. Para testar esse caminho no Linux,
  compile com `make CPPFLAGS=-DVM_USE_PTHREAD=1`.
- Os bytes são convertidos explicitamente para o intervalo -128 a 127,
  sem depender de `char` ser assinado na plataforma.
- Toda a entrada e o tamanho do backing são validados antes de abrir
  `correct.txt`. A saída usa diretamente `fopen`, `fprintf` e `fclose`, sem
  renomear ou remover arquivos. Erros de argumentos, entrada e backing preservam
  a saída anterior. Se houver falha durante a simulação ou a escrita, o programa
  retorna código diferente de zero; nesse caso, a saída pode estar incompleta.

## Arquivos

`vm.c` contém a validação, a tradução, as políticas e a busca com threads.
`Makefile` compila, limpa e executa os testes. `tests/test_vm.py` contém os
testes e o modelo independente. Este README documenta o uso e as decisões.

```sh
make clean
```

A limpeza remove o executável e as saídas geradas; preserva os dados de entrada.
O pacote `mavss.tar` do Classroom contém os fontes, Makefile, testes e
`evidencias.log`, dentro de `mavss/`. No GitHub, os arquivos de compilação
ficam na raiz para permitir a execução direta de `make`. Os dados fornecidos
na atividade e os binários compilados ficam fora das duas entregas.

Referência da atividade: Silberschatz, Galvin e Gagne, *Operating System
Concepts*, 10ª edição, projeto "Designing a Virtual Memory Manager", com as
alterações do enunciado da implementação 6.
