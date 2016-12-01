#include "qemu_debug.h"
#include "virerror.h"
#include "virlog.h"

#define MAX_STACK_FRAMES 64
#define NAME "/root/libvirt/libvirt-1.2.12/daemon/.libs/lt-libvirtd"
#define BUFSIZE 512

VIR_LOG_INIT("qemu.qemu_debug");

static void *stack_traces[MAX_STACK_FRAMES];

int addr2line(char const * const program_name, void const * const addr);
void posix_print_stack_trace(void);

int addr2line(char const * const program_name, void const * const addr)
{
  char addr2line_cmd[512] = {0};
  char buf[BUFSIZE];
  FILE *fp; 

  /* have addr2line map the address to the relent line in the code */
  #ifdef __APPLE__
    /* apple does things differently... */
    sprintf(addr2line_cmd,"atos -o %.256s %p", program_name, addr); 
  #else
    sprintf(addr2line_cmd,"addr2line -f -p -e %.256s %p", program_name, addr); 
  #endif
 
  /* This will print a nicely formatted string specifying the
 *      function and source line of the address */
  VIR_WARN("%s",addr2line_cmd);
  //

  if ((fp = popen(addr2line_cmd, "r")) == NULL) {
    VIR_WARN("Error opening pipe\n");
    return -1;
  }

  while (fgets(buf, BUFSIZE, fp) != NULL) {
    VIR_WARN("%s" ,buf);
  }

  if (pclose(fp)) {
    VIR_WARN("Command execution error");
    return -1;
  }

  return 0;
  //return system(addr2line_cmd);
}

void posix_print_stack_trace(void)
{
  int i, trace_size = 0;
  char **messages = (char **)NULL;
 
  trace_size = backtrace(stack_traces, MAX_STACK_FRAMES);
  messages = backtrace_symbols(stack_traces, trace_size);
  
  for (i = 0; i < trace_size; ++i)
  {
    if (addr2line(NAME, stack_traces[i]) != 0)
    {
      VIR_WARN("  error determining line # for: %s\n", messages[i]);
    }
 
  }
  if (messages) { free(messages); } 
}

void print_stack(void) {
	posix_print_stack_trace();
}
