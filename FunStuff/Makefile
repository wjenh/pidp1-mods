# Find all immediate subdirectories that contain a Makefile
SUBDIRS := $(dir $(wildcard */Makefile))

.PHONY: all clean $(SUBDIRS)

# Default target executed when you run 'make'
all: $(SUBDIRS)

# Rule to run make inside each subdirectory
$(SUBDIRS):
	$(MAKE) -C $@ all

# Clean target to recursively clean subdirectories
clean:
	@for dir in $(SUBDIRS); do \
		$(MAKE) -C $$dir clean; \
	done
