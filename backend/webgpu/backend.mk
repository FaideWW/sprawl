BACKEND_DIR := ../backend/webgpu
BACKEND_OBJ := $(patsubst $(BACKEND_DIR)/%.c,$(OBJ_DIR)/backend/%.c.o,$(wildcard $(BACKEND_DIR)/src/*.c))
CFLAGS_BACKEND := --use-port=emdawnwebgpu 
LDFLAGS += --use-port=emdawnwebgpu

SHADER_SRC := $(patsubst $(BACKEND_DIR)/shaders/%.wgsl,$(GEN_DIR)/%.wgsl.c,$(wildcard $(BACKEND_DIR)/shaders/*.wgsl))
SOURCES += $(SHADER_SRC)

$(OBJ_DIR)/backend/%.c.o: $(BACKEND_DIR)/%.c 
	@mkdir -p "$(@D)"
	$(CC) $(EMCCFLAGS) $(CFLAGS_BACKEND) -c "$<" -o "$@"

$(GEN_DIR)/%.wgsl.c: $(BACKEND_DIR)/shaders/%.wgsl
	@mkdir -p "$(@D)"
	xxd -i -n $(subst .,_,$(notdir $<)) "$<" > "$@"
