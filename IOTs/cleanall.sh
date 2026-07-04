for F in `find * -type d -not -name Tests`; do
    cd $F
    pwd
    make clean
    cd ..
done
