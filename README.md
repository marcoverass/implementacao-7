# Implementação 6 - Memória virtual

Identificação: Mavss.

O programa traduz endereços lógicos, consulta uma TLB de 16 entradas e carrega
páginas de 256 bytes sob demanda. A configuração padrão tem 128 frames e faz
a busca na TLB com 16 threads, uma para cada entrada.

## Compilação e execução

Requisitos: Linux, Unix ou macOS, compilador C com POSIX threads e `make`.
Os testes automatizados também usam Python 3. Não há bibliotecas Python externas.

Dentro do diretório `mavss`, coloque o `BACKING_STORE.bin` fornecido na atividade.
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
- As threads só leem as entradas da TLB. Cada uma tem seu próprio resultado,
  e a thread principal espera todas com `pthread_join` antes de atualizar a
  memória ou a TLB. Assim, a ordem de conclusão não altera a tradução.
- Os bytes são convertidos explicitamente para o intervalo -128 a 127,
  sem depender de `char` ser assinado na plataforma.
- Toda a entrada é validada antes da simulação. A saída é gravada em
  `correct.txt.tmp` e só substitui `correct.txt` depois de escrita e fechada
  com sucesso. Um erro retorna código diferente de zero e mensagem em stderr.

## Arquivos

`vm.c` contém a validação, a tradução, as políticas e a busca com pthreads.
`Makefile` compila, limpa e executa os testes. `tests/test_vm.py` contém os
testes e o modelo independente. Este README documenta o uso e as decisões.

```sh
make clean
```

A limpeza remove o executável e as saídas geradas; preserva os dados de entrada.
O pacote `mavss.tar` contém os fontes, Makefile, README e testes. Os dados
fornecidos na atividade e os binários compilados ficam fora desse pacote.

Referência da atividade: Silberschatz, Galvin e Gagne, *Operating System
Concepts*, 10ª edição, projeto "Designing a Virtual Memory Manager", com as
alterações do enunciado da implementação 6.
