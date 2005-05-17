/* pool.c */

/*
 * POOL Operations: used to manage the allocation of memory.  
 * In order to use a pool, the first element of each structure must be  
 * a char pointer "poolNext",  which maintain the pool lists.           
 */

#include <stdio.h>
#include <stdlib.h>
#include "pool.h"

/* PoolInit: create a pool of objects */
POOL* HPCC_PoolInit(int numObjects, int objSize)  
{

  char* ptr;
  int i;
  POOL* poolPtr;
  
  poolPtr = (POOL *) malloc (1 * sizeof (POOL));
			     
  poolPtr->head = NULL_PTR;                 /* Points to unallocated objects    */
  poolPtr->tail = NULL_PTR;                 /* Points to unallocated objects    */
  poolPtr->numObjs = numObjects+1;          /* Number of objects to allocate */
  poolPtr->objSize = objSize;               /* Size of each object              */
 
  ptr = (char*)malloc((poolPtr->numObjs)*(poolPtr->objSize)); /* Get a block of objects */
     
  if (ptr == NULL_PTR) { 
    fprintf(stdout,"Malloc fails in PoolInit\n");
    abort();
  }

  poolPtr->poolBase = ptr;
     
  for(i = 0; i<(poolPtr->numObjs)-1; i++) { /* link together the new objects*/
    *((char**)(ptr+i*(poolPtr->objSize))) = ptr+(i+1)*(poolPtr->objSize); /* setting up poolNext */
  }

  poolPtr->head = ptr;
  poolPtr->tail = ptr + (poolPtr->numObjs - 1)*(poolPtr->objSize);  /* adjust tail pointer  */
  *((char**)(poolPtr->tail)) = NULL_PTR; /* last object has no next object */

  return (poolPtr);
			     
}


char *HPCC_PoolGetObj(POOL* poolPtr)       
{
   char *ptr;

   if (poolPtr->head == NULL_PTR) {  
     fprintf(stdout,"No unallocated objects in pool\n");
     abort();
   }   
   ptr = poolPtr->head;                        
   poolPtr->head = *((char**)(poolPtr->head));  

   return ptr;   
}

void HPCC_PoolReturnObj(POOL *poolPtr,void *optr)       
{
   
   if (poolPtr->tail) {
     *((char **) (poolPtr->tail)) = (char*)optr;
     *((char **) ((char *)optr)) = NULL_PTR;
     poolPtr->tail = (char*)optr;
   }
   else {
     poolPtr->head = (char*)optr;
     poolPtr->tail = (char*)optr;
     *((char **) ((char *)optr)) = NULL_PTR;
   }

}

void HPCC_PoolFree(POOL* poolPtr)
{
  free(poolPtr->poolBase);
}
