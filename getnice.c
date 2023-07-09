#include "types.h"
#include "stat.h"
#include "user.h"

int main(int argc, char **argv) {

  if(argc != 2){
    printf(2, "Not enough arguments, getnice failed\n");
    exit();
  }

  int result = getnice(atoi(argv[1]));

  if(result == -1){
    printf(2, "Matching process id not found, getnice failed, terminated with %d\n", result);
  }
  else{
    printf(2, "process %d nice value is %d\n", atoi(argv[1]), result);
  }

  exit();
}