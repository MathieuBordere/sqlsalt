#CFLAGS += -std=c99 -Wall -Wextra -Werror -pedantic -Wshadow -Wformat -Wconversion \
-D_FORTIFY_SOURCE=2

SQLITE_TAG = version-3.46.0
SQLITE_SRC_DIR = sqlite-$(SQLITE_TAG)
SQLITE_BUILD_DIR = sqlite-build-$(SQLITE_TAG)

INCLUDES = -I$(SQLITE_BUILD_DIR)

ifdef DEBUG
	CFLAGS += -DSQLSALT_DEBUG -Og
else
	CFLAGS += -O2
endif
JOBS=$(shell echo $(MAKEFLAGS) | sed -n 's/.*\(-j[0-9]*\).*/\1/p')

SRC = sqlsalt.c
# Detect the OS and specify the compile command.
# TODO Windows, Android, iOS
OS := $(shell uname -s)
ifeq ($(OS),Linux)
	CC = gcc
	TARGET = libsqlsalt.so
	TARGET_TEST = libsqlsalttest.so 
	DYNAMIC_COMPILE = $(CC) -g $(CFLAGS) -fPIC -shared $(SRC) $(INCLUDES) -o $(TARGET)
	TEST_COMPILE = $(CC) -g $(CFLAGS) -DSQLSALT_STATIC -fPIC -shared $(SRC) $(INCLUDES) -o $(TARGET_TEST)
else ifeq ($(OS),Darwin)
	CC = clang
	TARGET = libsqlsalt.dylib
	TARGET_TEST = libsqlsalttest.dylib
	DYNAMIC_COMPILE = $(CC) -g $(CFLAGS) -fPIC -dynamiclib $(SRC) $(INCLUDES) -o $(TARGET)
# -undefined dynamic_lookup is needed, because some symbols will only be resolved once the library
# is linked against the SQLite testfixture. We can't link against a pre-built SQLite shared library
# for the TCL tests. On Linux this behavior is implied, and special options are not needed.
	TEST_COMPILE = $(CC) -g $(CFLAGS) -DSQLSALT_STATIC -fPIC -dynamiclib -undefined dynamic_lookup $(SRC) $(INCLUDES)\
	 -o $(TARGET_TEST)
endif

$(TARGET): sqlite $(SRC)
	@$(DYNAMIC_COMPILE)

.PHONY: test
test: $(TARGET_TEST)

$(TARGET_TEST): sqlite $(SRC)
	@$(TEST_COMPILE)

.PHONY: static
static: $(SRC)
	$(CC) -g $(CFLAGS) -DSQLSALT_STATIC -c $(SRC) -o sqlsalt.o
	ar rcs libsqlsalt.a sqlsalt.o

.PHONY: sqlite
sqlite: $(SQLITE_BUILD_DIR)
$(SQLITE_BUILD_DIR):
	rm -rf $(SQLITE_SRC_DIR) rm -rf $(SQLITE_BUILD_DIR)
	git clone --depth 1 --branch $(SQLITE_TAG) https://github.com/sqlite/sqlite.git $(SQLITE_SRC_DIR)
	mkdir $(SQLITE_BUILD_DIR) && cd $(SQLITE_BUILD_DIR) && ../$(SQLITE_SRC_DIR)/configure && make $(JOBS)

.PHONY: all
all: $(TARGET) $(TARGET_TEST) static

.PHONY: clean
clean:
	rm -rf sqlite* $(TARGET) sqlsalt.o libsqlsalt.* libsqlsalttest.*