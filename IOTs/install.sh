cd Lib; make
cd ..

for F in `find * -type d`; do
    cd $F
    pwd
    make install
    cd ..
done

strip --strip-unneeded *.so
