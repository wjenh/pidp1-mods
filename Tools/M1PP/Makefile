.PRECIOUS: *.c

m1pp: m1pp.l
	flex --outfile m1pp.c m1pp.l
	cc -o m1pp m1pp.c

clean:
	@rm m1pp.c m1pp 2>/dev/null || true
