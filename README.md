Test with: 

```
clear && clang -Weverything ejIPC-4.c buffer.c -o ejipc && echo -n hola | pv | ./ejipc "sed s/o/0/g| sed s/a/4/g"
```

```
clear && clang -fsanitize=address -fno-omit-frame-pointer ejIPC-4.c buffer.c -o ejipc && echo -n hola | pv | ./ejipc "sed s/o/0/g| sed s/a/4/g"
```

```
clear && clang -ggdb3 -O0 -std=c99 -fsanitize=address -fno-omit-frame-pointer -g ejIPC-4.c buffer.c -o ejipc &&
 echo -n hola | pv | ./ejipc "sed s/o/0/g| sed s/a/4/g"
```

```
clear && clang -g --std=c11 -pedantic -pedantic-errors -Wall -Wextra -Werror -Wno-unused-parameter -Wno-implicit-fallthrough -D_POSIX_C_SOURCE=200112L -ggdb3 -O0 -fsanitize=address -fno-omit-frame-pointer ejIPC-4.c buffer.c selector.c -o ejipc &&  echo -n hola | pv | ./ejipc "sed s/o/0/g| sed s/a/4/g"
```

Typical output:
```
4 B 0:00:00 [ 186KiB/s] [<=>                                                ]
h0l4
In parity: 0XA
Out parity: 0XA
```


Linux GCC: 

Typical output:
```
4 B 0:00:00 [ 186KiB/s] [<=>                                                ]
h0l4In parity: 0XA
Out parity: 0
```

```
gcc -c ejIPC-4.c buffer.c buffer.h ; gcc -o ejipc ejIPC-4.o buffer.o
```

```
echo -n hola | valgrind --leak-check=yes ./ejipc "sed s/o/0/g| sed s/a/4/g"
```

```
valgrind echo -n hola | pv | ./ejipc "sed s/o/0/g| sed s/a/4/g"
```

```
valgrind  --leak-check=full --show-leak-kinds=all echo -n hola | pv | ./ejipc "sed s/o/0/g| sed s/a/4/g"
```
