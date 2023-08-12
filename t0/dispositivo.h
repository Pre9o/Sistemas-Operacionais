#ifndef DISPOSITIVO_H
#define DISPOSITIVO_H

// simulador do relógio
// registra a passagem do tempo

#include "err.h"

typedef struct dispositivo_t dispositivo_t;

// cria e inicializa um relógio
// retorna NULL em caso de erro
dispositivo_t *disp_cria(void);

// destrói um relógio
// nenhuma outra operação pode ser realizada no relógio após esta chamada
void disp_destroi(dispositivo_t *self);

// registra a passagem de uma unidade de tempo
// esta função é chamada pelo controlador após a execução de cada instrução
void disp_random(dispositivo_t *self);

// retorna a hora atual do sistema, em unidades de tempo
// Funções para acessar o relógio como um dispositivo de E/S
//   só tem leitura, e dois dispositivos, '0' para ler o relógio local
//   (contador de instruções) e '1' para ler o relógio de tempo de CPU
//   consumido pelo simulador (em ms)
err_t disp_le(void *self, int dev, int *data);
#endif // RELOGIO_H
