#include "server.h"

int main(int argc, char *argv[]) {
  const char *configfile = (argc >= 2) ? argv[1] : NULL;
  return server_main(configfile);
}
