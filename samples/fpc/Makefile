# Build the Free Pascal hands-on twins against the vendored fbintf
# (git submodule update --init extern/fbintf; needs fpc 3.2+ and the
# Firebird client library).
#
#   make            # build everything into bin/
#   make bin/transactions && ./bin/transactions

FBINTF := ../../extern/fbintf/client
FPCOPTS := -Mdelphi -gl \
	-Fu$(FBINTF) -Fu$(FBINTF)/3.0 -Fu$(FBINTF)/3.0/firebird \
	-Fu$(FBINTF)/2.5 -Fu$(FBINTF)/2.5/include -Fi$(FBINTF)/include \
	-Fu. -FUunits -FEbin

PROGRAMS := $(patsubst %.pas,bin/%,$(filter-out FBHandsOn.pas,$(wildcard *.pas)))

all: $(PROGRAMS)

bin/%: %.pas FBHandsOn.pas | bin units
	fpc $(FPCOPTS) $<

bin units:
	mkdir -p $@

clean:
	rm -rf bin units

.PHONY: all clean
