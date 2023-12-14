#ifndef PROCESSO_H
#define PROCESSO_H
#include "err.h"

typedef enum estado_processo
{
  BLOQUEADO,
  PRONTO,
  EXECUTANDO
} estado_processo;

typedef enum dispositivo_bloqueado
{
  NENHUM,
  ESCRITA,
  LEITURA
} dispositivo_bloqueado;

typedef struct estado_cpu
{
  int registradorX;
  int registradorA;
  int registradorPC;
  err_t erro;
  int complemento;
  int modo;
} estado_cpu;

typedef struct processo_t
{
  int pid;
  char nome[100];
  estado_processo estado;
  int quantum;
  struct processo_t *esperando_processo;

  estado_cpu estado_cpu;
  dispositivo_bloqueado dispositivo_bloqueado;

  tabpag_t *tabpag;
} processo_t;

typedef struct tabela_processos_t
{
  processo_t *processos;
  int quantidade_processos;
} tabela_processos_t;

tabela_processos_t *inicia_tabela_processos();
void adiciona_novo_processo_na_tabela(tabela_processos_t *tabela_processos, char nome[100]);
void adiciona_processo_na_tabela(tabela_processos_t *tabela_processos, processo_t *processo);
processo_t *encontrar_processo_por_pid(tabela_processos_t *tabela, int targetPID);
processo_t *cria_processo(int pid, char nome[100], int estado);
processo_t *copia_processo(processo_t *processo);
processo_t *pega_proximo_processo_disponivel(tabela_processos_t *tabela);
bool remove_processo_tabela(tabela_processos_t *tabela, int targetPID);
int quantum();

#endif // PROCESSO_H