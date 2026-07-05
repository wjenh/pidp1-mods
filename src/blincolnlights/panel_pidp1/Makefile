INC=-I.. -I../pdp1 -IPinctrl
LIBS=-lpthread -lm
CFLAGS= $(INC) -O3 -Wno-unused-result

all: pinctrl newpanel

newpanel: pinctrl newpanel.o common.o configuration.o
	cc $(CFLAGS) -o newpanel newpanel.o common.o configuration.o Pinctrl/*.o $(LIBS)

newpanel.o: newpanel.c
	cc -c $(CFLAGS) newpanel.c

common.o: ../common.c
	cc -c $(CFLAGS) ../common.c

configuration.o: ../pdp1/configuration.c
	cc -c $(CFLAGS) $(INC) ../pdp1/configuration.c

run: newpanel
	./newpanel

pinctrl:
	cd Pinctrl; make

clean:
	@rm newpanel *.o >/dev/null 2>&1 || true
	cd Pinctrl; make clean
