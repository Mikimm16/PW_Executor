# PW_Executor
Program pozwalający na testowanie działania wielu programów jednocześnie.
Wykonany w ramach przedmiotu Programowanie Współbieżne.

Przykład użycia:
``` 
echo -e "foo\nbar" > in.txt;
echo -e "run cat in.txt\nsleep 100\nout 0" | ./executor 
```

