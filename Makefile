VERDI_HOME ?= $(shell echo $$VERDI_HOME)

# Check if VERDI_HOME is set
ifeq ($(VERDI_HOME),)
    $(error VERDI_HOME environment variable is not set)
endif

NPI_INC     = $(VERDI_HOME)/share/NPI/inc
NPI_L1_INC  = $(VERDI_HOME)/share/NPI/L1/C/inc
NPI_LIB     = $(VERDI_HOME)/share/NPI/lib/LINUX64

CXX         = g++
CXXFLAGS    = -Wall -std=c++11 -I$(NPI_INC) -I$(NPI_L1_INC) -Isrc
LDFLAGS     = -L$(NPI_LIB) -lNPI -lnpiL1 -ldl -lrt -lz

EXE         = xtrace
SRCS        = src/main.cpp \
              src/session/session_registry.cpp \
              src/session/session_manager.cpp \
              src/client/client.cpp \
              src/commands/cmd_session.cpp \
              src/commands/cmd_trace.cpp \
              src/server/server.cpp \
              src/control_dep/control_dep.cpp

OBJS        = $(SRCS:.cpp=.o)

all: $(EXE)

$(EXE): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f $(EXE) $(OBJS)

.PHONY: all clean
