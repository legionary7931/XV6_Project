#include "types.h"
#include "stat.h"
#include "user.h"
#include "param.h"
#include "fcntl.h"

int main(int argc, char **argv){
  int size = 4096;
  int fd = open("README", O_RDONLY);

  printf(1, "\n=====FREE MEMORY COUNT: %d =======\n", freemem());
  printf(1, "=====FILE MAPPING TEST=======\n");
  char* text = (char*)mmap(0, size, PROT_READ, MAP_POPULATE, fd, 0); 
  printf(1, "text:\n");
  for(int i=0; i<size; i++){
    printf(1, "%c", *(text+i));
  }

  printf(1, "\n=====FREE MEMORY COUNT: %d =======\n", freemem());
  char* text2 = (char*)mmap(size, size*2, PROT_READ, 0, fd, size); 
  //record mapping, trap occur * 2
  printf(1, "\n\ntext2:\n");
  for(int i=0; i<size; i++){
    printf(1, "%c", *(text2+i));
  }
  printf(1, "\n=====FREE MEMORY COUNT: %d =======\n", freemem());
  printf(1, "=====FILE MAPPING TEST COMPLETED =======\n");

  printf(1, "=====ANONYMOUS MAPPING TEST=======\n");
   char* text3 = (char*)mmap(size*3, size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS, -1, 0); //record mapping, trap occur
  printf(1, "\n\ntext3:\n");
  text3[200] = 'A'; //IF NO PROT_WRITE, panic error 
  for(int i=0; i<size; i++){
    printf(1, "%c", *(text3+i));
  }

  char* text4 = (char*)mmap(size*4, size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
  text4[200] = 'A'; //IF NO PROT_WRITE, panic error 
  printf(1, "\n\ntext4:\n");
  for(int i=0; i<size; i++){
    printf(1, "%c", *(text4+i));
  }
    

  printf(1, "\n=====FREE MEMORY COUNT: %d =======\n", freemem());
  printf(1, "=====ANONYMOUS MAPPING TEST COMPLETED =======\n");
  
  /*
  printf(1, "================= MUNMAP TEST ===============\n");
  printf(1, "Initial freememcount : %d\n", freemem());

  printf(1, "munmap result: %d\n", munmap(0x40000000 + size * 3));
  printf(1, "After unmapping text3, freememcount : %d\n", freemem());

  printf(1, "munmap result: %d\n", munmap(0x40000000 + size));
  printf(1, "After unmapping text2, freememcount : %d\n", freemem());
  printf(1, "================= MUNMAP TEST COMPLETED===============\n");
  */
  
  
  printf(1, "================= FORK TEST ==========================\n");
  int f;
  if((f=fork())==0){
    printf(1, "CHILD START\n");
    int result;

    printf(1,"\n#1 CHILD : unmapping text\n\n");
    result = munmap(0+0x40000000);
    printf(1,"unmap results: %d\n", result);
    printf(1,"freemem : %d\n",freemem());

    printf(1,"\n#2 CHILD : unmapping text2\n\n");
    result = munmap(size+0x40000000);
    printf(1,"unmap results: %d\n", result);
    printf(1,"freemem : %d\n",freemem());

    printf(1,"\n#3 CHILD : unmapping text3\n\n");
    result = munmap(size*3+0x40000000);
    printf(1,"unmap results: %d\n", result);
    printf(1,"freemem : %d\n",freemem());

    printf(1,"\n#4 CHILD : unmapping text3\n\n");
    result = munmap(size*4+0x40000000);
    printf(1,"unmap results: %d\n", result);
    printf(1,"freemem : %d\n",freemem());
    
    exit();
    return 0;
  }
  else{
    printf(1, "PARENT START\n");
    int result;

    printf(1,"\n#1 PARENT : unmapping text\n");
    result = munmap(0+0x40000000);
    printf(1,"unmap results: %d\n", result);
    printf(1,"freemem : %d\n",freemem());

    printf(1,"\n#2 PARENT : unmapping text2\n");
    result = munmap(size+0x40000000);
    printf(1,"\nunmap results: %d\n", result);
    printf(1,"freemem : %d\n",freemem());

    printf(1,"\n#3 PARENT : unmapping text3\n");
    result = munmap(size*3+0x40000000);
    printf(1,"unmap results: %d\n", result);
    printf(1,"freemem : %d\n",freemem());

    printf(1,"\n#4 PARENT : unmapping text3\n");
    result = munmap(size*4+0x40000000);
    printf(1,"unmap results: %d\n", result);
    printf(1,"freemem : %d\n",freemem());

    wait();
  }
  exit();
}