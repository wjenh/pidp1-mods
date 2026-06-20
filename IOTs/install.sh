cd Lib; make
cd ..

for F in `find * -type d -not -name Tests`; do
    echo Building $F
    cd $F
    pwd
    make install
    cd /opt/pidp1-mods/IOTs
done

strip --strip-unneeded *.so
