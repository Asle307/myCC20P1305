# Housekeeping only. Simulation and implementation stay in the Vivado GUI.
.DEFAULT_GOAL := help
.PHONY: help clean distclean
PYTHON ?= python3
DRY_RUN ?= 0
REPO_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
CLEAN_PREVIEW := $(if $(filter 1,$(DRY_RUN)),--dry-run,)

help:
	@printf '%s\n' 'make clean [DRY_RUN=1]      Remove transient Vivado/simulation output.' 'make distclean [DRY_RUN=1]  Also remove rebuildable implementation output.' 'Both preserve tracked files, vectors, .venv and artifacts. Close Vivado first.'

clean:
	@$(PYTHON) "$(REPO_DIR)script/clean.py" --mode clean $(CLEAN_PREVIEW)

distclean:
	@$(PYTHON) "$(REPO_DIR)script/clean.py" --mode distclean $(CLEAN_PREVIEW)
