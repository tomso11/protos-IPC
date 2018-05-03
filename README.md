Test with: 

```
clear && clang -Weverything ejIPC-4.c buffer.c -o ejipc && echo -n hola | pv | ./ejipc "sed s/o/0/g| sed s/a/4/g"
```

```
clear && clang -fsanitize=address -fno-omit-frame-pointer ejIPC-4.c buffer.c -o ejipc && echo -n hola | pv | ./ejipc "sed s/o/0/g| sed s/a/4/g"
```

Linux GCC: 

```
gcc -c ejIPC-4.c buffer.c buffer.h ; gcc -o ejipc ejIPC-4.o buffer.o
```

```
echo -n hola | valgrind --leak-check=yes ./ejipc "sed s/o/0/g| sed s/a/4/g"
```
