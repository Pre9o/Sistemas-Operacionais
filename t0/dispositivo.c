#include "dispositivo.h"
#include <stdlib.h>
#include <time.h>

struct dispositivo_t {
  int agora;
};

dispositivo_t *disp_cria(void)
{
  dispositivo_t *self;
  self = malloc(sizeof(dispositivo_t));
  if (self != NULL) {
    self->agora = 0;
  }
  return self;
}

void disp_destroi(dispositivo_t *self)
{
  free(self);
}

void disp_random(dispositivo_t *self)
{
  self->agora += rand() % 10;
}

err_t disp_le(void *self, int dev, int *data)
{
  err_t err = ERR_OK;
  switch (dev) {
    case 0:
      *data = ((dispositivo_t*)self)->agora;
      break;
    default: 
      err = ERR_END_INV;
  }
  return err;

}


