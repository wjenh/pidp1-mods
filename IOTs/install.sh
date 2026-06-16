cd Lib; make
cd ..

for F in `find * -type d`; do
    echo Building $F
    cd $F
    pwd
    make install
    cd /opt/pidp1-mods/IOTs
done

strip --strip-unneeded *.so
