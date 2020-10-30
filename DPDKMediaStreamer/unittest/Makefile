
%.out:	%.cpp
	gcc -g $^ -o $@

all:	*.cpp
	for i in $< ; do make --always-make $${i%.cpp}.out && ./$${i%.cpp}.out 2>&- | while read l ; do echo $${i%.cpp}: $$l ; done ; done

