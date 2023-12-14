#include "so.h"
#include "irq.h"
#include "programa.h"
#include "processo.h"
#include "instrucao.h"

#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

// intervalo entre interrupções do relógio
#define INTERVALO_INTERRUPCAO 50 // em instruções executadas

int id_processo_executando = -1;

struct so_t
{
  cpu_t* cpu;
  mem_t* mem;
  console_t* console;
  relogio_t* relogio;
  tabela_processos_t* tabela_processos;
};

// função de tratamento de interrupção (entrada no SO)
static err_t so_trata_interrupcao(void* argC, int reg_A);

// funções auxiliares
static int so_carrega_programa(so_t* self, char* nome_do_executavel);
static bool copia_str_da_mem(int tam, char str[tam], mem_t* mem, int ender);

so_t* so_cria(cpu_t* cpu, mem_t* mem, console_t* console, relogio_t* relogio)
{
  so_t* self = malloc(sizeof(*self));
  if (self == NULL)
    return NULL;

  self->cpu = cpu;
  self->mem = mem;
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

  return self;
}

void so_destroi(so_t* self)
{
  cpu_define_chamaC(self->cpu, NULL, NULL);
  free(self);
}

// Tratamento de interrupção

// funções auxiliares para tratar cada tipo de interrupção
static err_t so_trata_irq(so_t* self, int irq);
static err_t so_trata_irq_reset(so_t* self);
static err_t so_trata_irq_err_cpu(so_t* self);
static err_t so_trata_irq_relogio(so_t* self);
static err_t so_trata_irq_desconhecida(so_t* self, int irq);
static err_t so_trata_chamada_sistema(so_t* self);

// funções auxiliares para o tratamento de interrupção
// static void so_salva_estado_da_cpu(so_t* self);
static void so_trata_pendencias(so_t* self);
static void so_escalona(so_t* self);

// funções Pedro Ramos :)
processo_t* so_cria_processo(so_t* self, char nome[100]);
void so_salva_estado_cpu_no_processo(so_t* self);
void so_carrega_estado_processo_na_cpu(so_t* self);
void pode_desbloquear_entrada_saida(so_t* self, processo_t* processo);
bool pode_desbloquear_por_espera(so_t* self, processo_t* processo);

// Chamadas de sistema

static void so_chamada_le(so_t* self);
static void so_chamada_escr(so_t* self);
static void so_chamada_cria_proc(so_t* self);
static void so_chamada_mata_proc(so_t* self);
static void so_chamada_espera_proc(so_t* self);

// função a ser chamada pela CPU quando executa a instrução CHAMAC
// essa instrução só deve ser executada quando for tratar uma interrupção
// o primeiro argumento é um ponteiro para o SO, o segundo é a identificação
//   da interrupção
// na inicialização do SO é colocada no endereço 10 uma rotina que executa
//   CHAMAC; quando recebe uma interrupção, a CPU salva os registradores
//   no endereço 0, e desvia para o endereço 10
static err_t so_trata_interrupcao(void* argC, int reg_A)
{
  so_t* self = argC;
  irq_t irq = reg_A;
  err_t err;
  console_printf(self->console, "SO: recebi IRQ %d (%s)", irq, irq_nome(irq));
  // salva o estado da cpu no descritor do processo que foi interrompido
  // so_salva_estado_da_cpu(self);
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

void so_salva_estado_cpu_no_processo(so_t* self) {
  processo_t* processo_atual = encontrar_processo_por_pid(self->tabela_processos, id_processo_executando);
  if (processo_atual == NULL) {
    return;
  }

  console_printf(self->console, "SO: Salva estado da cpu no processo %s", processo_atual->nome);
  mem_le(self->mem, IRQ_END_X, &processo_atual->estado_cpu.registradorX);
  mem_le(self->mem, IRQ_END_A, &processo_atual->estado_cpu.registradorA);
  mem_le(self->mem, IRQ_END_PC, &processo_atual->estado_cpu.registradorPC);
  mem_le(self->mem, IRQ_END_complemento, &processo_atual->estado_cpu.complemento);
  int leitura_memoria;
  mem_le(self->mem, IRQ_END_erro, &leitura_memoria);
  processo_atual->estado_cpu.erro = leitura_memoria;
}

void so_carrega_estado_processo_na_cpu(so_t* self) {
  processo_t* processo_atual = encontrar_processo_por_pid(self->tabela_processos, id_processo_executando);
  if (processo_atual == NULL) {
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

void pode_desbloquear_entrada_saida(so_t* self, processo_t* processo) {
  int verifica_terminal = (processo->pid * 4) + 1;

  int estado;
  term_le(self->console, verifica_terminal, &estado);

  if (!estado){
    return;
  }

  if (processo->dispositivo_bloqueado == LEITURA) {
    int terminal_de = (processo->pid * 4) + 2;
    term_le(self->console, terminal_de, &processo->estado_cpu.registradorX);
    processo->estado = PRONTO;
    processo->dispositivo_bloqueado = NENHUM;
  }
  else if (processo->dispositivo_bloqueado == ESCRITA) {
    int terminal_ee = (processo->pid * 4) + 3;
    term_escr(self->console, terminal_ee, processo->estado_cpu.registradorX);
    processo->estado = PRONTO;
    processo->dispositivo_bloqueado = NENHUM;
  }
}


bool pode_desbloquear_por_espera(so_t* self, processo_t* processo) {
  processo_t* processo_esperado = processo->esperando_processo;

  if (processo_esperado != NULL){
    processo_t *processo_tabela = encontrar_processo_por_pid(self->tabela_processos, processo_esperado->pid);
    if(processo_tabela != NULL){
      return false;
    }
  }
  return true;

}

static void so_trata_pendencias(so_t* self)
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
    processo_t* processo = &self->tabela_processos->processos[i];
    if (processo->estado == BLOQUEADO)
    {
      if (processo->dispositivo_bloqueado != NENHUM){
        console_printf(self->console, "AAAAAAAAAAAAAAAAAAAAAAAA: processo %s bloqueado, esperando terminal", processo->nome);
        pode_desbloquear_entrada_saida(self, processo);
      }
      
      if (pode_desbloquear_por_espera(self, processo)) {
        console_printf(self->console, "BBBBBBBBBBBBBBBBBBBBBBB: processo %s bloqueado, esperando processo", processo->nome);
        processo->esperando_processo = NULL;
        
    }
    }
  }
  // Atualiza as contabilidades
}
static void so_escalona(so_t* self)
{
  processo_t* processo_executando = encontrar_processo_por_pid(self->tabela_processos, id_processo_executando);

  if (processo_executando == NULL)
  {
    if (self->tabela_processos->quantidade_processos == 0)
    {
      id_processo_executando = -1;
      return;
    }
    processo_t* proximo_processo = pega_proximo_processo_disponivel(self->tabela_processos);
    if (proximo_processo == NULL) {
      mem_escreve(self->mem, IRQ_END_erro, ERR_CPU_PARADA);
      mem_escreve(self->mem, IRQ_END_modo, usuario);
      return;
    }
    id_processo_executando = proximo_processo->pid;
    proximo_processo->estado = EXECUTANDO;
  }
  else
  {
    processo_t* processo_copia = copia_processo(processo_executando);
    if (processo_copia->estado == PRONTO || processo_copia->estado == BLOQUEADO)
    {
      if (processo_copia->estado == BLOQUEADO) {
        char so_message[200];
        sprintf(so_message, "SO: processo %s bloqueado, escalonando processo...", processo_copia->nome);
        console_printf(self->console, so_message);
      }
      else {
        char so_message[200];
        sprintf(so_message, "SO: quantum do processo: %s expirado, escalonando processo...", processo_copia->nome);
        console_printf(self->console, so_message);
      }
      // joga o processo pro fim da fila
      remove_processo_tabela(self->tabela_processos, processo_copia->pid);
      adiciona_processo_na_tabela(self->tabela_processos, processo_copia);
      processo_t* proximo_processo = pega_proximo_processo_disponivel(self->tabela_processos);
      if (proximo_processo == NULL) {
        id_processo_executando = -1;
        mem_escreve(self->mem, IRQ_END_erro, ERR_CPU_PARADA);
        return;
      }
      id_processo_executando = proximo_processo->pid;
      proximo_processo->estado = EXECUTANDO;
      if (proximo_processo->pid == processo_copia->pid) {
        so_salva_estado_cpu_no_processo(self);
      }
    }
    else {
      so_salva_estado_cpu_no_processo(self);
    }
  }
}

static err_t so_trata_irq(so_t* self, int irq)
{
  err_t err;
  console_printf(self->console, "SO: recebi IRQ %d (%s)", irq, irq_nome(irq));
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

static err_t so_trata_irq_reset(so_t* self)
{
  // coloca um programa na memória
  char nome_programa[100] = "init.maq";
  int ender = so_carrega_programa(self, nome_programa);
  processo_t* processo_adicionado = so_cria_processo(self, nome_programa);

  if (ender != 100)
  {
    console_printf(self->console, "SO: problema na carga do programa inicial");
    return ERR_CPU_PARADA;
  }
  processo_adicionado->estado_cpu.registradorPC = ender;
  processo_adicionado->estado_cpu.modo = usuario;

  mem_escreve(self->mem, IRQ_END_modo, usuario);
  return ERR_OK;
}

static err_t so_trata_irq_err_cpu(so_t* self)
{
  // Ocorreu um erro interno na CPU
  // O erro está codificado em IRQ_END_erro
  // Em geral, causa a morte do processo que causou o erro
  // Ainda não temos processos, causa a parada da CPU
  int err_int;
  // com suporte a processos, deveria pegar o valor do registrador erro
  //   no descritor do processo corrente, e reagir de acordo com esse erro
  //   (em geral, matando o processo)

  processo_t* processo_atual = encontrar_processo_por_pid(self->tabela_processos, id_processo_executando);

  if (processo_atual != NULL) {
    err_int = processo_atual->estado_cpu.erro;
    err_t err = err_int;
    if (err != ERR_OK) {
      console_printf(self->console, "SO: IRQ tratada, erro na execução, eliminando processo: %s", processo_atual->nome);
      so_chamada_mata_proc(self);
      return ERR_OK;
    }
  }

  mem_le(self->mem, IRQ_END_erro, &err_int);
  err_t err = err_int;

  console_printf(self->console,
    "SO: IRQ nao tratada -- erro na CPU: %s", err_nome(err));
  return ERR_OK;
}

static err_t so_trata_irq_relogio(so_t* self)
{
  // ocorreu uma interrupção do relógio
  // rearma o interruptor do relógio e reinicializa o timer para a próxima interrupção
  rel_escr(self->relogio, 3, 0); // desliga o sinalizador de interrupção
  rel_escr(self->relogio, 2, INTERVALO_INTERRUPCAO);
  // trata a interrupção
  // por exemplo, decrementa o quantum do processo corrente, quando se tem
  // um escalonador com quantum
  processo_t* processo_atual = encontrar_processo_por_pid(self->tabela_processos, id_processo_executando);
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

static err_t so_trata_irq_desconhecida(so_t* self, int irq)
{
  console_printf(self->console,
    "SO: nao sei tratar IRQ %d (%s)", irq, irq_nome(irq));
  return ERR_CPU_PARADA;
}

static err_t so_trata_chamada_sistema(so_t* self)
{
  // com processos, a identificação da chamada está no reg A no descritor
  //   do processo
  processo_t* processo_atual = encontrar_processo_por_pid(self->tabela_processos, id_processo_executando);
  if (processo_atual == NULL) {
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

static void so_chamada_le(so_t* self)
{
  int terminal_de = (id_processo_executando * 4) + 2;
  int terminal_el = (id_processo_executando * 4) + 1;

  int estado;
  term_le(self->console, terminal_el, &estado);

  processo_t* processo_atual = encontrar_processo_por_pid(self->tabela_processos, id_processo_executando);
  if (processo_atual == NULL) {
    return;
  }

  // Verifica se o terminal está em uso
  if (estado == 0) {
    // Se o terminal estiver sendo usado por outro processo, bloqueie este processo
    processo_atual->estado = BLOQUEADO;
    processo_atual->dispositivo_bloqueado = LEITURA;
  } else {
    // Se o terminal não estiver em uso, prossiga com a leitura e marque o terminal como em uso
    term_le(self->console, terminal_de, &processo_atual->estado_cpu.registradorX);
  }
}

void so_chamada_escr(so_t* self) {
  int terminal_de = (id_processo_executando * 4) + 2;
  int terminal_ee = (id_processo_executando * 4) + 3;

  int estado;
  term_le(self->console, terminal_ee, &estado);

  processo_t* processo_atual = encontrar_processo_por_pid(self->tabela_processos, id_processo_executando);
  if (processo_atual == NULL) {
    return;
  }

  // Verifica se o terminal está em uso
  if (estado == 0) {
    // Se o terminal estiver sendo usado por outro processo, bloqueie este processo
    processo_atual->estado = BLOQUEADO;
    processo_atual->dispositivo_bloqueado = ESCRITA;
    console_printf(self->console, "SO: AAAAAAAAAA processo %s bloqueado, esperando terminal", processo_atual->nome);
  } else {
    // Se o terminal não estiver em uso, prossiga com a escrita e marque o terminal como em uso
    term_escr(self->console, terminal_de, processo_atual->estado_cpu.registradorX);
  }
}

processo_t* so_cria_processo(so_t* self, char nome[100])
{
  //TODO: escrever o pid do processo criado no A do criador
  adiciona_novo_processo_na_tabela(self->tabela_processos, nome);
  processo_t* processo_carregado = &self->tabela_processos->processos[self->tabela_processos->quantidade_processos - 1];

  char so_message[200];
  sprintf(so_message, "SO: Processo criado Nome: %s PID: %d", processo_carregado->nome, processo_carregado->pid);

  console_printf(self->console, so_message);
  return processo_carregado;
}

static void so_chamada_cria_proc(so_t* self)
{
  console_printf(self->console, "SO: chamada cria processo");
  // em X está o endereço onde está o nome do arquivo
  processo_t* processo_atual = encontrar_processo_por_pid(self->tabela_processos, id_processo_executando);
  int ender_proc = processo_atual->estado_cpu.registradorX;
  // deveria ler o X do descritor do processo criador
  char nome[100];
  if (copia_str_da_mem(100, nome, self->mem, ender_proc))
  {
    int ender_carga = so_carrega_programa(self, nome);
    processo_t* processo_criado = so_cria_processo(self, nome);

    if (ender_carga > 0)
    {
      // deveria escrever no PC do descritor do processo criado
      processo_criado->estado_cpu.registradorPC = ender_carga;
      processo_atual->estado_cpu.registradorA = processo_criado->pid;
      mem_escreve(self->mem, IRQ_END_A, processo_criado->pid);
      return;
    }
  }
  // deveria escrever -1 (se erro) ou 0 (se OK) no reg A do processo que
  //   pediu a criação
  processo_atual->estado_cpu.registradorA = -1;
}

static void so_chamada_espera_proc(so_t* self) {
  console_printf(self->console, "SO: chamada espera processo");
  processo_t* processo_esperador = encontrar_processo_por_pid(self->tabela_processos, id_processo_executando);
  int pid_processo_esperado = processo_esperador->estado_cpu.registradorX;

  // verificar se processo esperado já acabou antes de colocar ele como esperados
  if (encontrar_processo_por_pid(self->tabela_processos, pid_processo_esperado) == NULL) {
    console_printf(self->console, "SO: processo %d já terminou de executar, liberando processo %s", pid_processo_esperado, processo_esperador->nome);
    return;
  }
  processo_t* processo_esperado = &self->tabela_processos->processos[pid_processo_esperado];

  processo_esperador->esperando_processo = processo_esperado;
  processo_esperador->estado = BLOQUEADO;
  console_printf(self->console, "SO: processo %s BLOQUEADO, esperando processo %s", processo_esperador->nome, processo_esperado->nome);
}

static void so_chamada_mata_proc(so_t* self)
{
  processo_t* processo_atual = encontrar_processo_por_pid(self->tabela_processos, id_processo_executando);
  if (processo_atual == NULL) {
    return;
  }
  console_printf(self->console, "SO: Removendo processo %s PID: %d da tabela", processo_atual->nome, processo_atual->pid);
  remove_processo_tabela(self->tabela_processos, id_processo_executando);
}

// carrega o programa na memória
// retorna o endereço de carga ou -1
static int so_carrega_programa(so_t* self, char* nome_do_executavel)
{
  // programa para executar na nossa CPU
  programa_t* prog = prog_cria(nome_do_executavel);
  if (prog == NULL)
  {
    console_printf(self->console,
      "Erro na leitura do programa '%s'\n", nome_do_executavel);
    return -1;
  }

  int end_ini = prog_end_carga(prog);
  int end_fim = end_ini + prog_tamanho(prog);

  for (int end = end_ini; end < end_fim; end++)
  {
    if (mem_escreve(self->mem, end, prog_dado(prog, end)) != ERR_OK)
    {
      console_printf(self->console,
        "Erro na carga da memoria, endereco %d\n", end);
      return -1;
    }
  }
  prog_destroi(prog);
  console_printf(self->console,
    "SO: carga de '%s' em %d-%d", nome_do_executavel, end_ini, end_fim);
  return end_ini;
}

// copia uma string da memória do simulador para o vetor str.
// retorna false se erro (string maior que vetor, valor não ascii na memória,
//   erro de acesso à memória)
static bool copia_str_da_mem(int tam, char str[tam], mem_t* mem, int ender)
{
  for (int indice_str = 0; indice_str < tam; indice_str++)
  {
    int caractere;
    if (mem_le(mem, ender + indice_str, &caractere) != ERR_OK)
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
