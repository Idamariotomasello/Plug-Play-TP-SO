include settings.mk

################################################################################

outname = bin/$(1)

define compile_out
	$(CC) $(CFLAGS) -o "$@" $^ $(IDIRS:%=-I%) $(LIBDIRS:%=-L%) $(RUNDIRS:%=-Wl,-rpath,%) $(LIBS:%=-l%)
endef

define compile_objs
	$(CC) $(CFLAGS) -c -o "$@" $< $(IDIRS:%=-I%)
endef

################################################################################

NAME=$(shell pwd | xargs -I{} basename "{}")

CC=gcc
AR=ar
ARFLAGS=rcs
MAKE=make --no-print-directory

# Fuentes: .c en src/, .h en src/ e include/
SRCS_C += $(shell find src -iname "*.c")
SRCS_H += $(shell find src include -iname "*.h" 2>/dev/null)

DEPS = $(foreach SHL,$(SHARED_LIBPATHS),$(SHL:%=%/lib/lib$(notdir $(SHL)).so)) \
	$(foreach STL,$(STATIC_LIBPATHS),$(STL:%=%/lib/lib$(notdir $(STL)).a))

# Incluye tanto /src como /include de cada dependencia y del módulo actual
IDIRS += $(addsuffix /src,$(SHARED_LIBPATHS) $(STATIC_LIBPATHS) .) \
         $(addsuffix /include,$(SHARED_LIBPATHS) $(STATIC_LIBPATHS) .) \
         /usr/local/include

LIBDIRS = $(addsuffix /lib,$(SHARED_LIBPATHS) $(STATIC_LIBPATHS)) /usr/local/lib

RUNDIRS = $(SHARED_LIBPATHS:%=$(shell pwd)/%/lib)

OBJS = $(patsubst src/%.c,obj/%.o,$(SRCS_C))

OUT = $(call outname,$(NAME))

TESTS_DIR=tests
TESTS_C += $(shell find $(TESTS_DIR)/ -iname "*.c" 2>/dev/null)
TESTS_H += $(shell find $(TESTS_DIR)/ -iname "*.h" 2>/dev/null)
TEST_OBJS = $(filter-out $(TEST_EXCLUDE), $(TESTS_C)) \
            $(patsubst src/%.c,obj/%.o,$(filter-out $(TEST_EXCLUDE), $(SRCS_C)))
TEST = bin/$(NAME)_tests

.PHONY: all
all: debug

.PHONY: debug
debug: CFLAGS = $(CDEBUG)
debug: $(OUT)

.PHONY: release
release: CFLAGS = $(CRELEASE)
release: $(OUT)

.PHONY: test
test: CFLAGS = $(CDEBUG)
test: $(TEST)

.PHONY: clean
clean:
	-rm -rfv $(dir $(TEST) $(OBJS) $(OUT))
	-for dir in $(SHARED_LIBPATHS) $(STATIC_LIBPATHS); do $(MAKE) -C $$dir clean; done

$(OUT): $(OBJS) | $(dir $(OUT))
	$(call compile_out)

$(TEST): $(TEST_OBJS) $(DEPS) | $(dir $(TEST))
	$(CC) $(CFLAGS) -o "$@" $^ $(IDIRS:%=-I%) $(LIBDIRS:%=-L%) $(RUNDIRS:%=-Wl,-rpath,%) $(LIBS:%=-l%) -lcspecs

obj/%.o: src/%.c $(SRCS_H) $(DEPS) | $(dir $(OBJS))
	$(call compile_objs)

# Busca fuentes en src/ e include/ de la dependencia al compilarla recursivamente
.SECONDEXPANSION:
$(DEPS): $$(shell find \
		$$(patsubst %lib/,%src/,$$(dir $$@)) \
		$$(patsubst %lib/,%include/,$$(dir $$@)) \
		-iname "*.c" -or -iname "*.h" 2>/dev/null)
	$(MAKE) -C $(patsubst %lib/,%,$(dir $@)) 3>&1 1>&2 2>&3 \
		| sed -E 's,(src/)[^ ]+\.(c|h)\:,$(patsubst %lib/,%,$(dir $@))&,' \
		3>&2 2>&1 1>&3

$(sort $(dir $(OUT) $(OBJS))):
	mkdir -pv $@