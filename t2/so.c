#include "so.h"
#include "irq.h"
#include "programa.h"
#include "processo.h"
#include "instrucao.h"
#include "tabpag.h"

#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

// intervalo entre interrupções do relógio
#define INTERVALO_INTERRUPCAO 50 // em instruções executadas

int id_processo_executando = -1;

// Não tem processos nem memória virtual, mas é preciso usar a paginação,
//   pelo menos para implementar relocação, já que os programas estão sendo
//   todos montados para serem executados no endereço 0 e o endereço 0
//   físico é usado pelo hardware nas interrupções.
// Os programas vão ser carregados no início de um quadro, e usar quantos
//   quadros forem necessárias. Para isso a variável quadro_livre vai conter
//   o número do primeiro quadro da memória principal que ainda não foi usado.
//   Na carga do processo, a tabela de páginas (deveria ter uma por processo,
//   mas não tem processo) é alterada para que o endereço virtual 0 resulte
//   no quadro onde o programa foi carregado.

struct so_t
{
  cpu_t *cpu;
  mem_t *mem;
  mem_t *mem_secundaria;
  mmu_t *mmu;
  console_t *console;
  relogio_t *relogio;
  tabela_processos_t *tabela_processos;
  // quando tiver memória virtual, o controle de memória livre e ocupada
  //   é mais completo que isso
  int quadro_livre;
  // quando tiver processos, não tem essa tabela aqui, tem que tem uma para
  //   cada processo
  tabpag_t *tabpag;
};

// função de tratamento de interrupção (entrada no SO)
static err_t so_trata_interrupcao(void *argC, int reg_A);

// funções auxiliares
static int so_carrega_programa(so_t *self, char *nome_do_executavel, processo_t *processo);
static bool so_copia_str_do_processo(so_t *self, int tam, char str[tam],
                                     int end_virt, processo_t *processo);

so_t *so_cria(cpu_t *cpu, mem_t *mem, mem_t *mem_secundaria, mmu_t *mmu,
              console_t *console, relogio_t *relogio)
{
  so_t *self = malloc(sizeof(*self));
  if (self == NULL)
    return NULL;

  self->cpu = cpu;
  self->mem = mem;
  self->mem_secundaria = mem_secundaria;
  self->mmu = mmu;
  self->console = console;
  self->relogio = relogio;
  self->tabela_processos = inicia_tabela_processos();

  // quando a CPU executar uma instrução CHAMAC, deve chamar a função
  //   so_trata_interrupcao
  cpu_define_chamaC(self->cpu, so_trata_interrupcao, self);

  // coloca o tratador de interrupção na memória
  // quando a CPU aceita uma interrupção, passa para modo supervisor,
  //   salva seu estado à partir do endereço 0, e desvia para o endereço 10
  // colocamos no endereço 10 a instrução CHAMAC, que vai chamar
  //   so_trata_interrupcao (conforme foi definido acima) e no endereço 11
  //   colocamos a instrução RETI, para que a CPU retorne da interrupção
  //   (recuperando seu estado no endereço 0) depois que o SO retornar de
  //   so_trata_interrupcao.
  mem_escreve(self->mem, 10, CHAMAC);
  mem_escreve(self->mem, 11, RETI);

  // programa o relógio para gerar uma interrupção após INTERVALO_INTERRUPCAO
  rel_escr(self->relogio, 2, INTERVALO_INTERRUPCAO);

  // inicializa a tabela de páginas global, e entrega ela para a MMU
  // com processos, essa tabela não existiria, teria uma por processo
  self->tabpag = tabpag_cria();
  mmu_define_tabpag(self->mmu, self->tabpag);
  // define o primeiro quadro livre de memória como o seguinte àquele que
  //   contém o endereço 99 (as 100 primeiras posições de memória (pelo menos)
  //   não vão ser usadas por programas de usuário)
  self->quadro_livre = 99 / TAM_PAGINA + 1;
  return self;
}

void so_destroi(so_t *self)
{
  cpu_define_chamaC(self->cpu, NULL, NULL);
  free(self);
}

// Tratamento de interrupção

// funções auxiliares para tratar cada tipo de interrupção
static err_t so_trata_irq(so_t *self, int irq);
static err_t so_trata_irq_reset(so_t *self);
static err_t so_trata_irq_err_cpu(so_t *self);
static err_t so_trata_irq_relogio(so_t *self);
static err_t so_trata_irq_desconhecida(so_t *self, int irq);
static err_t so_trata_chamada_sistema(so_t *self);

// funções auxiliares para o tratamento de interrupção
static void so_trata_pendencias(so_t *self);
static void so_escalona(so_t *self);

// funções Pedro Ramos :)
processo_t *so_cria_processo(so_t *self, char nome[100]);
void so_salva_estado_cpu_no_processo(so_t *self);
void so_carrega_estado_processo_na_cpu(so_t *self);
bool pode_desbloquear(so_t *self, processo_t *processo);

// Chamadas de sistema

static void so_chamada_le(so_t *self);
static void so_chamada_escr(so_t *self);
static void so_chamada_cria_proc(so_t *self);
static void so_chamada_mata_proc(so_t *self);
static void so_chamada_espera_proc(so_t *self);

// função a ser chamada pela CPU quando executa a instrução CHAMAC
// essa instrução só deve ser executada quando for tratar uma interrupção
// o primeiro argumento é um ponteiro para o SO, o segundo é a identificação
//   da interrupção
// na inicialização do SO é colocada no endereço 10 uma rotina que executa
//   CHAMAC; quando recebe uma interrupção, a CPU salva os registradores
//   no endereço 0, e desvia para o endereço 10
static err_t so_trata_interrupcao(void *argC, int reg_A)
{
  so_t *self = argC;
  irq_t irq = reg_A;
  err_t err;
  console_printf(self->console, "SO: recebi IRQ %d (%s)", irq, irq_nome(irq));
  // salva o estado da cpu no descritor do processo que foi interrompido
  so_salva_estado_cpu_no_processo(self);
  // faz o atendimento da interrupção
  err = so_trata_irq(self, irq);
  // faz o processamento independente da interrupção
  so_trata_pendencias(self);
  // escolhe o próximo processo a executar
  so_escalona(self);
  // recupera o estado do processo escolhido
  so_carrega_estado_processo_na_cpu(self);
  return err;
}

void so_salva_estado_cpu_no_processo(so_t *self)
{
  processo_t *processo_atual = encontrar_processo_por_pid(self->tabela_processos, id_processo_executando);
  if (processo_atual == NULL)
    return;
  console_printf(self->console, "SO: Salva estado da cpu no processo %s", processo_atual->nome);
  mem_le(self->mem, IRQ_END_X, &processo_atual->estado_cpu.registradorX);
  mem_le(self->mem, IRQ_END_A, &processo_atual->estado_cpu.registradorA);
  mem_le(self->mem, IRQ_END_PC, &processo_atual->estado_cpu.registradorPC);
  mem_le(self->mem, IRQ_END_complemento, &processo_atual->estado_cpu.complemento);
  int leitura_memoria;
  mem_le(self->mem, IRQ_END_erro, &leitura_memoria);
  processo_atual->estado_cpu.erro = leitura_memoria;
}

void so_carrega_estado_processo_na_cpu(so_t *self)
{
  processo_t *processo_atual = encontrar_processo_por_pid(self->tabela_processos, id_processo_executando);
  if (processo_atual == NULL)
  {
    mem_escreve(self->mem, IRQ_END_erro, ERR_CPU_PARADA);
    return;
  }
  console_printf(self->console, "SO: Carrega estado do processo %s na cpu", processo_atual->nome);
  mem_escreve(self->mem, IRQ_END_X, processo_atual->estado_cpu.registradorX);
  mem_escreve(self->mem, IRQ_END_A, processo_atual->estado_cpu.registradorA);
  mem_escreve(self->mem, IRQ_END_PC, processo_atual->estado_cpu.registradorPC);
  mem_escreve(self->mem, IRQ_END_erro, processo_atual->estado_cpu.erro);
  mem_escreve(self->mem, IRQ_END_complemento, processo_atual->estado_cpu.complemento);
  mem_escreve(self->mem, IRQ_END_modo, processo_atual->estado_cpu.modo);
}

bool pode_desbloquear(so_t *self, processo_t *processo)
{
  int terminal_el = (id_processo_executando * 4) + 1;
  int terminal_ee = (id_processo_executando * 4) + 3;

  int estado_escrita;
  int estado_leitura;
  term_le(self->console, terminal_el, &estado_leitura);
  term_le(self->console, terminal_ee, &estado_escrita);

  dispositivo_bloqueado dispositivo = processo->dispositivo_bloqueado;
  processo_t *processo_esperado = processo->esperando_processo;
  if (processo_esperado != NULL)
  {
    processo_t *processo_tabela = encontrar_processo_por_pid(self->tabela_processos, processo_esperado->pid);
    if (processo_tabela != NULL)
      return false;
  }

  if (dispositivo == ESCRITA && estado_escrita == 0)
  {
    return false;
  }

  if (dispositivo == LEITURA && estado_leitura == 0)
  {
    return false;
  }

  return true;
}

static void so_trata_pendencias(so_t *self)
{
  // realiza ações que não são diretamente ligadas com a interrupção que
  // está sendo atendida:
  // - E/S pendente
  // - desbloqueio de processos
  // - contabilidades

  // Verifica se há E/S pendente

  // Verifica se há processos bloqueados
  for (int i = 0; i < self->tabela_processos->quantidade_processos; i++)
  {
    processo_t *processo = &self->tabela_processos->processos[i];
    if (processo->estado == BLOQUEADO)
    {
      // Se houver, verifica se o processo pode ser desbloqueado
      if (pode_desbloquear(self, processo))
      {
        // Se o processo pode ser desbloqueado, atualiza o seu estado
        processo->estado = PRONTO;
        processo->dispositivo_bloqueado = NENHUM;
        processo->esperando_processo = NULL;
      }
    }
  }
}

static void so_escalona(so_t *self)
{
  processo_t *processo_executando = encontrar_processo_por_pid(self->tabela_processos, id_processo_executando);

  if (processo_executando == NULL)
  {
    if (self->tabela_processos->quantidade_processos == 0)
    {
      id_processo_executando = -1;
      return;
    }
    processo_t *proximo_processo = pega_proximo_processo_disponivel(self->tabela_processos);
    if (proximo_processo == NULL)
    {
      mem_escreve(self->mem, IRQ_END_erro, ERR_CPU_PARADA);
      return;
    }
    id_processo_executando = proximo_processo->pid;
    proximo_processo->estado = EXECUTANDO;
    mmu_define_tabpag(self->mmu, proximo_processo->tabpag);
  }
  else
  {
    processo_t *processo_copia = copia_processo(processo_executando);
    if (processo_copia->estado == PRONTO || processo_copia->estado == BLOQUEADO)
    {
      if (processo_copia->estado == BLOQUEADO)
      {
        char so_message[200];
        sprintf(so_message, "SO: processo %s bloqueado, escalonando processo...", processo_copia->nome);
        console_printf(self->console, so_message);
      }
      else
      {
        char so_message[200];
        sprintf(so_message, "SO: quantum do processo: %s expirado, escalonando processo...", processo_copia->nome);
        console_printf(self->console, so_message);
      }
      // joga o processo pro fim da fila
      remove_processo_tabela(self->tabela_processos, processo_copia->pid);
      adiciona_processo_na_tabela(self->tabela_processos, processo_copia);
      processo_t *proximo_processo = pega_proximo_processo_disponivel(self->tabela_processos);
      if (proximo_processo == NULL)
      {
        id_processo_executando = -1;
        mem_escreve(self->mem, IRQ_END_erro, ERR_CPU_PARADA);
        return;
      }
      id_processo_executando = proximo_processo->pid;
      proximo_processo->estado = EXECUTANDO;
      mmu_define_tabpag(self->mmu, proximo_processo->tabpag);
      if (proximo_processo->pid == processo_copia->pid)
      {
        so_salva_estado_cpu_no_processo(self);
      }
    }
    else
    {
      so_salva_estado_cpu_no_processo(self);
    }
  }
}

static err_t so_trata_irq(so_t *self, int irq)
{
  err_t err;
  switch (irq)
  {
  case IRQ_RESET:
    err = so_trata_irq_reset(self);
    break;
  case IRQ_ERR_CPU:
    err = so_trata_irq_err_cpu(self);
    break;
  case IRQ_SISTEMA:
    err = so_trata_chamada_sistema(self);
    break;
  case IRQ_RELOGIO:
    err = so_trata_irq_relogio(self);
    break;
  default:
    err = so_trata_irq_desconhecida(self, irq);
  }
  return err;
}

static err_t so_trata_irq_reset(so_t *self)
{
  // coloca um programa na memória
  char nome_programa[100] = "init.maq";
  processo_t *processo_adicionado = so_cria_processo(self, nome_programa);
  int ender = so_carrega_programa(self, nome_programa, processo_adicionado);

  if (ender < 0)
  {
    console_printf(self->console, "SO: problema na carga do programa inicial");
    return ERR_CPU_PARADA;
  }
  processo_adicionado->estado_cpu.registradorPC = ender;
  processo_adicionado->estado_cpu.modo = usuario;

  mem_escreve(self->mem, IRQ_END_modo, usuario);
  return ERR_OK;
}

static err_t so_trata_irq_err_cpu(so_t *self)
{
  // Ocorreu um erro interno na CPU
  // O erro está codificado em IRQ_END_erro
  // Em geral, causa a morte do processo que causou o erro
  // Ainda não temos processos, causa a parada da CPU
  int err_int;
  // com suporte a processos, deveria pegar o valor do registrador erro
  //   no descritor do processo corrente, e reagir de acordo com esse erro
  //   (em geral, matando o processo)

  processo_t *processo_atual = encontrar_processo_por_pid(self->tabela_processos, id_processo_executando);

  if (processo_atual != NULL)
  {
    err_int = processo_atual->estado_cpu.erro;
    err_t err = err_int;
    if (err != ERR_OK)
    {
      console_printf(self->console, "SO: IRQ tratada, erro na execução, eliminando processo: %s", processo_atual->nome);
      so_chamada_mata_proc(self);
      return ERR_OK;
    }
    return ERR_OK;
  }

  mem_le(self->mem, IRQ_END_erro, &err_int);
  err_t err = err_int;

  console_printf(self->console,
                 "SO: IRQ nao tratada -- erro na CPU: %s", err_nome(err));
  return ERR_CPU_PARADA;
}

static err_t so_trata_irq_relogio(so_t *self)
{
  // ocorreu uma interrupção do relógio
  // rearma o interruptor do relógio e reinicializa o timer para a próxima interrupção
  rel_escr(self->relogio, 3, 0); // desliga o sinalizador de interrupção
  rel_escr(self->relogio, 2, INTERVALO_INTERRUPCAO);
  // trata a interrupção
  // por exemplo, decrementa o quantum do processo corrente, quando se tem
  // um escalonador com quantum
  processo_t *processo_atual = encontrar_processo_por_pid(self->tabela_processos, id_processo_executando);
  if (processo_atual != NULL)
  {
    processo_atual->quantum--;
    if (processo_atual->quantum == 0)
    {
      processo_atual->estado = PRONTO;
      processo_atual->quantum = quantum();
    }
  }
  console_printf(self->console, "SO: interrupcao do relogio");
  return ERR_OK;
}

static err_t so_trata_irq_desconhecida(so_t *self, int irq)
{
  console_printf(self->console,
                 "SO: não sei tratar IRQ %d (%s)", irq, irq_nome(irq));
  return ERR_CPU_PARADA;
}

static err_t so_trata_chamada_sistema(so_t *self)
{
  // com processos, a identificação da chamada está no reg A no descritor
  //   do processo
  processo_t *processo_atual = encontrar_processo_por_pid(self->tabela_processos, id_processo_executando);
  if (processo_atual == NULL)
  {
    return ERR_CPU_PARADA;
  }
  int id_chamada = processo_atual->estado_cpu.registradorA;
  console_printf(self->console,
                 "SO: chamada de sistema %d", id_chamada);
  switch (id_chamada)
  {
  case SO_LE:
    so_chamada_le(self);
    break;
  case SO_ESCR:
    so_chamada_escr(self);
    break;
  case SO_CRIA_PROC:
    so_chamada_cria_proc(self);
    break;
  case SO_MATA_PROC:
    so_chamada_mata_proc(self);
    break;
  case SO_ESPERA_PROC:
    so_chamada_espera_proc(self);
    break;
  default:
    console_printf(self->console,
                   "SO: chamada de sistema desconhecida (%d)", id_chamada);
    return ERR_CPU_PARADA;
  }
  return ERR_OK;
}

static void so_chamada_le(so_t *self)
{
  // leitura
  int terminal_dl = (id_processo_executando * 4) + 0;
  // estado leitura
  int terminal_el = (id_processo_executando * 4) + 1;
  for (;;)
  {
    int estado;
    term_le(self->console, terminal_el, &estado);
    if (estado != 0)
    {
      break;
    }
    // processo_t* processo_atual = encontrar_processo_por_pid(self->tabela_processos, id_processo_executando);
    // if (processo_atual != NULL) {
    //   processo_atual->estado = BLOQUEADO;
    //   processo_atual->dispositivo_bloqueado = LEITURA;
    // }
    console_tictac(self->console);
    console_atualiza(self->console);
  }
  int dado;
  term_le(self->console, terminal_dl, &dado);

  processo_t *processo_atual = encontrar_processo_por_pid(self->tabela_processos, id_processo_executando);
  processo_atual->estado_cpu.registradorA = dado;
}

static void so_chamada_escr(so_t *self)
{
  // implementação com espera ocupada
  //   deveria bloquear o processo se dispositivo ocupado
  // implementação escrevendo direto do terminal A
  //   deveria usar dispositivo corrente de saída do processo

  // escrita
  int terminal_de = (id_processo_executando * 4) + 2;
  // estado escrita
  int terminal_ee = (id_processo_executando * 4) + 3;

  for (;;)
  {
    int estado;
    term_le(self->console, terminal_ee, &estado);
    if (estado != 0)
      break;
    // processo_t* processo_atual = encontrar_processo_por_pid(self->tabela_processos, id_processo_executando);
    // if (processo_atual != NULL) {
    //   processo_atual->estado = BLOQUEADO;
    //   processo_atual->dispositivo_bloqueado = ESCRITA;
    // }
    console_tictac(self->console);
    console_atualiza(self->console);
  }
  processo_t *processo_atual = encontrar_processo_por_pid(self->tabela_processos, id_processo_executando);
  if (processo_atual == NULL)
  {
    return;
  }
  term_escr(self->console, terminal_de, processo_atual->estado_cpu.registradorX);
  processo_atual->estado_cpu.registradorA = 0;
}

processo_t *so_cria_processo(so_t *self, char nome[100])
{
  // TODO: escrever o pid do processo criado no A do criador
  adiciona_novo_processo_na_tabela(self->tabela_processos, nome);
  processo_t *processo_carregado = &self->tabela_processos->processos[self->tabela_processos->quantidade_processos - 1];

  char so_message[200];
  sprintf(so_message, "SO: Processo criado Nome: %s PID: %d", processo_carregado->nome, processo_carregado->pid);

  console_printf(self->console, so_message);
  return processo_carregado;
}

static void so_chamada_cria_proc(so_t *self)
{
  console_printf(self->console, "SO: chamada cria processo");
  // em X está o endereço onde está o nome do arquivo
  processo_t *processo_atual = encontrar_processo_por_pid(self->tabela_processos, id_processo_executando);
  int ender_proc = processo_atual->estado_cpu.registradorX;
  // deveria ler o X do descritor do processo criador
  char nome[100];

  // copia_str_da_mem(100, nome, self->mem, ender_proc)
  if (so_copia_str_do_processo(self, 100, nome, ender_proc, processo_atual))
  {
    processo_t *processo_criado = so_cria_processo(self, nome);
    int ender_carga = so_carrega_programa(self, nome, processo_criado);

    // deveria escrever no PC do descritor do processo criado
    processo_criado->estado_cpu.registradorPC = ender_carga;
    processo_atual->estado_cpu.registradorA = processo_criado->pid;
    mem_escreve(self->mem, IRQ_END_A, processo_criado->pid);
    return;
  }
  // deveria escrever -1 (se erro) ou 0 (se OK) no reg A do processo que
  //   pediu a criação
  processo_atual->estado_cpu.registradorA = -1;
}

static void so_chamada_espera_proc(so_t *self)
{
  console_printf(self->console, "SO: chamada espera processo");
  processo_t *processo_esperador = encontrar_processo_por_pid(self->tabela_processos, id_processo_executando);
  int pid_processo_esperado = processo_esperador->estado_cpu.registradorX;
  // int pid_processo_esperado = 1;

  // verificar se processo esperado já acabou antes de colocar ele como esperados
  if (encontrar_processo_por_pid(self->tabela_processos, pid_processo_esperado) == NULL)
  {
    console_printf(self->console, "SO: processo %d já terminou de executar, liberando processo %s", pid_processo_esperado, processo_esperador->nome);
    return;
  }
  processo_t *processo_esperado = &self->tabela_processos->processos[pid_processo_esperado];

  processo_esperador->esperando_processo = processo_esperado;
  processo_esperador->estado = BLOQUEADO;
  console_printf(self->console, "SO: processo %s BLOQUEADO, esperando processo %s", processo_esperador->nome, processo_esperado->nome);
}

static void so_chamada_mata_proc(so_t *self)
{
  processo_t *processo_atual = encontrar_processo_por_pid(self->tabela_processos, id_processo_executando);
  if (processo_atual == NULL)
  {
    return;
  }
  console_printf(self->console, "SO: Removendo processo %s PID: %d da tabela", processo_atual->nome, processo_atual->pid);
  remove_processo_tabela(self->tabela_processos, id_processo_executando);
}

// carrega o programa na memória
// retorna o endereço de carga ou -1
// está simplesmente lendo para o próximo quadro que nunca foi ocupado,
//   nem testa se tem memória disponível
// com memória virtual, a forma mais simples de implementar a carga
//   de um programa é carregá-lo para a memória secundária, e mapear
//   todas as páginas da tabela de páginas como inválidas. assim,
//   as páginas serão colocadas na memória principal por demanda.
//   para simplificar ainda mais, a memória secundária pode ser alocada
//   da forma como a principal está sendo alocada aqui (sem reuso)
static int so_carrega_programa(so_t *self, char *nome_do_executavel, processo_t *processo)
{
  // programa para executar na nossa CPU
  programa_t *prog = prog_cria(nome_do_executavel);
  if (prog == NULL)
  {
    console_printf(self->console,
                   "Erro na leitura do programa '%s'\n", nome_do_executavel);
    return -1;
  }

  int end_virt_ini = prog_end_carga(prog);
  int end_virt_fim = end_virt_ini + prog_tamanho(prog) - 1;
  int pagina_ini = end_virt_ini / TAM_PAGINA;
  int pagina_fim = end_virt_fim / TAM_PAGINA;
  int quadro_ini = self->quadro_livre;
  // mapeia as páginas nos quadros
  int quadro = quadro_ini;
  for (int pagina = pagina_ini; pagina <= pagina_fim; pagina++)
  {
    tabpag_define_quadro(processo->tabpag, pagina, quadro);
    quadro++;
  }
  self->quadro_livre = quadro;

  mmu_define_tabpag(self->mmu, processo->tabpag);

  // carrega o programa na memória principal
  int end_fis_ini = quadro_ini * TAM_PAGINA;
  int end_fis = end_fis_ini;
  for (int end_virt = end_virt_ini; end_virt <= end_virt_fim; end_virt++)
  {
    if (mem_escreve(self->mem, end_fis, prog_dado(prog, end_virt)) != ERR_OK)
    {
      console_printf(self->console,
                     "Erro na carga da memória, end virt %d fís %d\n", end_virt, end_fis);
      return -1;
    }
    end_fis++;
  }
  prog_destroi(prog);
  console_printf(self->console,
                 "SO: carga de '%s' em V%d-%d F%d-%d", nome_do_executavel,
                 end_virt_ini, end_virt_fim, end_fis_ini, end_fis - 1);
  return end_virt_ini;
}

// copia uma string da memória do processo para o vetor str.
// retorna false se erro (string maior que vetor, valor não ascii na memória,
//   erro de acesso à memória)
// o endereço é um endereço virtual de um processo.
// Com processos e memória virtual implementados, esta função deve também
//   receber o processo como argumento
// Cada valor do espaço de endereçamento do processo pode estar em memória
//   principal ou secundária
// O endereço é um endereço virtual de um processo.
// Com processos e memória virtual implementados, esta função deve também
//   receber o processo como argumento
// Com memória virtual, cada valor do espaço de endereçamento do processo
//   pode estar em memória principal ou secundária
static bool so_copia_str_do_processo(so_t *self, int tam, char str[tam],
                                     int end_virt, processo_t *processo)
{
  // TODO: pegar tabela de páginas do processo e usar aqui
  for (int indice_str = 0; indice_str < tam; indice_str++)
  {
    int caractere;
    // não tem memória virtual implementada, posso usar a mmu para traduzir
    //   os endereços e acessar a memória
    if (mmu_le(self->mmu, end_virt + indice_str, &caractere, usuario) != ERR_OK)
    {
      return false;
    }
    if (caractere < 0 || caractere > 255)
    {
      return false;
    }
    str[indice_str] = caractere;
    if (caractere == 0)
    {
      return true;
    }
  }
  // estourou o tamanho de str
  return false;
}
