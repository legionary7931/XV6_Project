#include "types.h"
#include "stat.h"
#include "user.h"

int main(int argc, char **argv) {
  if(argc != 3){
    printf(2, "Not enough arguments, setnice failed\n");
    exit();
  }

  int result;

  
  printf(2, "set process %d nice value to %d\n", atoi(argv[1]), atoi(argv[2]));
  result =  setnice(atoi(argv[1]), atoi(argv[2]));
  

  if(result == 0){
    printf(2, "setnice return value: %d, setnice success\n", result);
  }
  else if(result == -1){
    printf(2, "setnice return value: %d, setnice failed\n", result);
  }
  else{
    printf(2, "unexpected return value, terminated with %d\n", result);
  }
  

  exit();
}