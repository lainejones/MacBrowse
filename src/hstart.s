| hstart.s - entry point of MacHFS-Handler.  Linked first, with -nostartfiles:
| a handler must not run libnix's startup, which would take the DOS startup
| packet for a Workbench message.
	.text
	.globl	_start
_start:
	jmp	_handler_main
