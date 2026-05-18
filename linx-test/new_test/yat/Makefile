XCC := clang
XCXX := clang++
PYTHON := python3

TEST_VERBOSE_SWITCH := -DTEST_VERBOSE=0
XCCFLAGS := --target=linx64 -march=linx64im -mabi=lp64 -O3 $(TEST_VERBOSE_SWITCH)
XCXXFLAGS := --target=linx64 -march=linx64im -mabi=lp64 -O3 --std=c++17 $(TEST_VERBOSE_SWITCH)

bin_imm :=  $(patsubst asm/%.S,%,$(filter %i_t.S %i_r.S %iw_t.S %iw_r.S, $(wildcard asm/*.S)))
src_imm := $(patsubst %,src/%.cpp,$(bin_imm))
obj_imm := $(patsubst %,obj/%.o,$(bin_imm))
bin := $(filter-out $(bin_imm), $(patsubst evt/%.evt,%,$(wildcard evt/*.evt)))
obj := $(patsubst %,obj/%.o,$(bin))
data := $(patsubst %,data/%.dat,$(bin))
src := $(patsubst %,src/%.c,$(bin))

all: dir $(bin) sl $(bin_imm) sl_sp mem

.PHONY: dir
dir:
	if [ ! -e obj ]; then mkdir obj; fi
	if [ ! -e data ]; then mkdir data; fi
	if [ ! -e bin ]; then mkdir bin; fi
	if [ ! -e src ]; then mkdir src; fi

sl: sl.c asm/load.S asm/store.S
	$(XCC) $(XCCFLAGS) -I./asm -w $^ -o bin/$@

sl_sp: sl_sp.c asm/sl_sp.S
	$(XCC) $(XCCFLAGS) -I./asm -w $^ -o bin/$@

mem: mem.c asm/fentry.S asm/fexit.S asm/ftexit.S asm/mempush.S asm/mempop.S
	$(XCC) $(XCCFLAGS) -I./asm -w $^ -o bin/$@

$(bin_imm): %: src/%.cpp obj/%.o
	$(XCXX) $(XCXXFLAGS) -w -I. $^ -o bin/$@

$(bin): %: src/%.c obj/%.o
	$(XCC) $(XCCFLAGS) -I. $^ -o bin/$@

$(src_imm):
	echo '' > $(patsubst %.cpp,%.dec,$@)
	echo '' > $(patsubst %.cpp,%.act,$@)
	m="$(patsubst src/%.cpp,%,$@)"; f=$$(cat "imm/$${m}.evt" | awk -F"[(]|(,[ ]?)|[)]" '{ print $$4 }'); cat "imm/$${f}" | while read imm; do nmm=$$(echo $${imm} | sed -e 's/-/m/'); cpp "imm/$${m}.evt" -P -D"NEW_EVENT(x,y,_)=FUNC_DECL(x,$${nmm})" >> "src/$${m}.dec"; cat "imm/data.dat" | while read dat; do cpp "imm/$${m}.evt" -P -D"NEW_EVENT(x,y,_)=_test(x,y,$${dat},$${imm},$${nmm});" >> "src/$${m}.act"; done; done; sed -e "s/DEC_FILE/\"src\/$${m}.dec\"/" -e "s/ACT_FILE/\"src\/$${m}.act\"/" -e "s/INST_NAME/blk_$${m}/" main_imm.cpp > $@

$(src): src/%.c: main.c evt/%.evt data/%.dat
	sed -e 's/#TEST_EVENT/#include "evt\/$(patsubst src/%.c,%.evt,$@)"/' -e 's/#TEST_DATA/#include "data\/$(patsubst src/%.c,%.dat,$@)"/' $< > $@

$(data): data/%.dat: gen/%.py
	$(PYTHON) $< > $@

$(obj_imm): obj/%.o: imm/%.evt asm/%.S
	cpp asm/block_type.h -P > "$(patsubst obj/%.o,src/%.S,$@)"
	f=$$(cat "$(patsubst obj/%.o,imm/%.evt,$@)" | awk -F"[(]|(,[ ]?)|[)]" '{ print $$4 }'); cat "imm/$${f}" | while read imm; do nmm=$$(echo $${imm} | sed -e 's/-/m/'); cpp "$(patsubst obj/%.o,asm/%.S,$@)" -P -D"NMM=$${nmm}" -D"IMMEDIATE=$${imm}" -D"INST_IMM(h,i)=_INST_IMM(h,i)" -D"_INST_IMM(h,i)=h##_##i" | sed -e 's/_-/_m/g' >> "$(patsubst obj/%.o,src/%.S,$@)"; done
	$(XCC) $(XCCFLAGS) -c $(patsubst obj/%.o,src/%.S,$@) -o $@

$(obj): obj/%.o: asm/%.S
	$(XCC) $(XCCFLAGS) -c $< -o $@

.PHONY: clean
clean:
	rm -f bin/*
	rm -f obj/*
	rm -f data/*
	rm -f src/*
	rm -rf gen/__pycache__/*
