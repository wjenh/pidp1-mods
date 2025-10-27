all: drumloader.rim rotate.rim drumdiag.rim drumtest.rim

drumloader.rim: drumloader.mac
	../../bin/macro1_1 -r drumloader.mac

rotate.rim: rotate.mac
	../../bin/macro1_1 -r rotate.mac

drumtest.rim: drumtest.mac
	../../bin/macro1_1 drumtest.mac

drumdiag.rim: drumdiag.mac
	../../bin/macro1_1 drumdiag.mac

clean:
	rm *.rim
	rm *.lst
