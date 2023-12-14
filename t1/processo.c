#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uuid/uuid.h>
#include <stdbool.h>
#include "err.h"

int QUANTUM = 1;

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
  struct processo_t* esperando_processo;

  estado_cpu estado_cpu;
  dispositivo_bloqueado dispositivo_bloqueado;
} processo_t;

typedef struct tabela_processos_t
{
  processo_t* processos;
  int quantidade_processos;
} tabela_processos_t;

void generate_uuid_str(char* uuid_str)
{
  uuid_t uuid;
  uuid_generate(uuid);

  uuid_unparse(uuid, uuid_str);
}

processo_t* cria_processo(int pid, char nome[100], int estado)
{
  processo_t* novo_processo = (processo_t*)malloc(sizeof(processo_t));
  if (novo_processo == NULL)
  {
    return NULL;
  }

  strncpy(novo_processo->nome, nome, sizeof(novo_processo->nome));
  novo_processo->pid = pid;
  novo_processo->estado = estado;
  novo_processo->quantum = QUANTUM;
  novo_processo->esperando_processo = NULL;
  novo_processo->estado_cpu.registradorX = 0;
  novo_processo->estado_cpu.registradorA = 0;
  novo_processo->estado_cpu.registradorPC = 0;
  novo_processo->estado_cpu.complemento = 0;
  novo_processo->dispositivo_bloqueado = NENHUM;
  novo_processo->estado_cpu.modo = 1; // usuário
  novo_processo->estado_cpu.erro = ERR_OK;
  return novo_processo;
}

processo_t* copia_processo(processo_t* processo)
{
  processo_t* novo_processo = (processo_t*)malloc(sizeof(processo_t));
  if (novo_processo == NULL)
  {
    return NULL;
  }

  strncpy(novo_processo->nome, processo->nome, sizeof(novo_processo->nome));
  novo_processo->pid = processo->pid;
  novo_processo->estado = processo->estado;
  novo_processo->quantum = processo->quantum;
  novo_processo->esperando_processo = processo->esperando_processo;
  novo_processo->estado_cpu.registradorX = processo->estado_cpu.registradorX;
  novo_processo->estado_cpu.registradorA = processo->estado_cpu.registradorA;
  novo_processo->estado_cpu.registradorPC = processo->estado_cpu.registradorPC;
  novo_processo->estado_cpu.complemento = processo->estado_cpu.complemento;
  novo_processo->estado_cpu.modo = processo->estado_cpu.modo; // usuário
  novo_processo->estado_cpu.erro = processo->estado_cpu.erro;
  novo_processo->dispositivo_bloqueado = processo->dispositivo_bloqueado;
  return novo_processo;
}

tabela_processos_t* inicia_tabela_processos()
{
  tabela_processos_t* tabela_processos = (tabela_processos_t*)malloc(sizeof(tabela_processos_t));
  tabela_processos->processos = NULL;
  tabela_processos->quantidade_processos = 0;

  return tabela_processos;
}


void adiciona_novo_processo_na_tabela(tabela_processos_t* tabela_processos, char nome[100])
{

  if (tabela_processos == NULL)
  {
    return;
  }

  int pid;

  if (tabela_processos->quantidade_processos == 0)
  {
    pid = 0;
  }
  else
  {
    pid = tabela_processos->processos[tabela_processos->quantidade_processos - 1].pid + 1;
  }

  processo_t* novo_processo = cria_processo(pid, nome, PRONTO);
  if (novo_processo == NULL)
  {
    return;
  }

  processo_t* novo_array_processos = (processo_t*)realloc(tabela_processos->processos, (tabela_processos->quantidade_processos + 1) * sizeof(processo_t));
  if (novo_array_processos == NULL)
  {
    return;
  }

  tabela_processos->processos = novo_array_processos;
  tabela_processos->processos[tabela_processos->quantidade_processos] = *novo_processo;
  tabela_processos->quantidade_processos++;
}


void adiciona_processo_na_tabela(tabela_processos_t* tabela_processos, processo_t* processo) {
  if (tabela_processos == NULL)
  {
    return;
  }

  processo_t* novo_array_processos = (processo_t*)realloc(tabela_processos->processos, (tabela_processos->quantidade_processos + 1) * sizeof(processo_t));
  if (novo_array_processos == NULL)
  {
    return;
  }

  tabela_processos->processos = novo_array_processos;
  tabela_processos->processos[tabela_processos->quantidade_processos] = *processo;
  tabela_processos->quantidade_processos++;
}

processo_t* encontrar_processo_por_pid(tabela_processos_t* tabela, int targetPID)
{
  for (int i = 0; i < tabela->quantidade_processos; i++)
  {
    if (tabela->processos[i].pid == targetPID)
    {
      return &(tabela->processos[i]);
    }
  }

  return NULL;
}

processo_t* pega_proximo_processo_disponivel(tabela_processos_t* tabela) {
  for (int i = 0; i < tabela->quantidade_processos; i++)
  {
    if (tabela->processos[i].estado == PRONTO)
    {
      return &(tabela->processos[i]);
    }
  }

  return NULL;
}

bool remove_processo_tabela(tabela_processos_t* tabela, int targetPID)
{
  for (int i = 0; i < tabela->quantidade_processos; i++)
  {
    if (tabela->processos[i].pid == targetPID)
    {
      for (int j = i; j < tabela->quantidade_processos - 1; j++)
      {
        tabela->processos[j] = tabela->processos[j + 1];
      }
      tabela->quantidade_processos--;
      return true;
    }
  }

  return false;
}

int quantum() {
  return QUANTUM;
}
