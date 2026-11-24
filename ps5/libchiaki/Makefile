SDK=/sessions/rcw-014mybzjynwowyjd27croker/b/pacbrew/opt/ps5-payload-sdk
CC=$(SDK)/bin/prospero-clang
AR=$(SDK)/bin/prospero-ar
HB=/sessions/rcw-014mybzjynwowyjd27croker/b/pacbrew/opt/ps5-payload-sdk/target/user/homebrew
C=/sessions/rcw-014mybzjynwowyjd27croker/b/chiaki-ng
TP=/sessions/rcw-014mybzjynwowyjd27croker/b/chiaki-ng/third-party
CFLAGS=-O2 -w -include ps5compat.h -I include -I $(C)/lib/include -I gen -I $(TP)/nanopb -I $(TP)/jerasure/include -I $(TP)/gf-complete/include -I $(HB)/include -I $(HB)/include/opus -DPB_C99_STATIC_ASSERT -DCHIAKI_VERSION_MAJOR=1 -DCHIAKI_VERSION_MINOR=0 -DCHIAKI_VERSION_PATCH=0 -DCHIAKI_VERSION=\"ps5\" -DCHIAKI_LIB_ENABLE_OPUS -DCHIAKI_PS5
LIBSRC=$(addprefix $(C)/lib/,$(shell cat libsrc.txt)) gen/takion.pb.c net_stubs.c
TPSRC=$(TP)/nanopb/pb_common.c $(TP)/nanopb/pb_decode.c $(TP)/nanopb/pb_encode.c $(wildcard $(TP)/gf-complete/src/gf*.c) $(TP)/jerasure/src/galois.c $(TP)/jerasure/src/jerasure.c $(TP)/jerasure/src/reed_sol.c $(TP)/jerasure/src/cauchy.c $(TP)/jerasure/src/liberation.c
OBJ=$(patsubst %.c,obj/%.o,$(notdir $(LIBSRC) $(TPSRC)))
vpath %.c $(sort $(dir $(LIBSRC) $(TPSRC)))
libchiaki.a: $(OBJ)
	$(AR) rcs $@ $^
obj/%.o: %.c
	@mkdir -p obj
	$(CC) $(CFLAGS) -c $< -o $@
