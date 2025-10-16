#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>     
#include <unistd.h>     

#include <common.h>

char* emucmd(const char *);

int
main(int argc, char **argv)
{
char *answerP; 
char cmd[16];
float fval;

    if( argc == 3 )
    {
        if( strcmp(argv[1], "alpha") && strcmp(argv[1], "gain") && strcmp(argv[1], "tuning") )
        {
            fprintf(stderr,"Usage: pdp1audio [alpha|gain|tuning] value\n");
            exit(1);
        }
    }
    else if( argc != 2 )
    {
        fprintf(stderr,"Usage: pdp1audio on/off\n");
        exit(1);
    }

    if( argc == 3 )
    {
        fval = atof(argv[2]);
        sprintf(cmd,"audio %s %f", argv[1], fval);
    }
    else
    {
        sprintf(cmd,"audio %s", argv[1]);
    }

    answerP = emucmd(cmd);
    if( !answerP )
    {
        printf("Can't connect to pidp1!\n");
    }
    else
    {
        printf("%s\n", answerP);
    }

    exit(0);
}

char*
emucmd(const char *cmd)
{
	static char reply[1024];

	int emu = dial("localhost", 1040);
	if(emu < 0) {
		fprintf(stderr, "error: couldn't connct to localhost:1040\n");
		return nil;
	}

	int n = write(emu, cmd, strlen(cmd));
	if(n <= 0) {
		close(emu);
		return nil;
	}

	n = read(emu, reply, sizeof(reply)-1);
	if(n <= 0) {
		close(emu);
		return nil;
	}

	reply[n] = '\0';
	close(emu);
	return reply;
}
