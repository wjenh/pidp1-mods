for F in `find * -type d`; do
    cd $F
    pwd
    make clean
    cd ..
done
