# Suite-level ctest discovery for OloEngine-Tests (run with `cmake -P` as a
# POST_BUILD step of the target; see the wiring in OloEngine/tests/CMakeLists.txt).
#
# WHY NOT gtest_discover_tests FOR EVERYTHING. That module registers every gtest
# case as its own ctest entry, so every case is its own process. Measured on the
# self-hosted Linux runners (ASan, run 34686730577, 2026-09-12): 8,109 cases,
# 111 min of summed case time, and 7,785 of the cases take under one second --
# 83 of those 111 minutes is the engine's per-process start-up (0.64 s median,
# the same 0.62 s an EMPTY run costs on the Windows dev box), paid once per
# case. The cases that do real work are a few hundred, and they are the ones
# OLO_HEAVY_TESTS and OLO_MESH_CACHE_TESTS keep on the per-case path.
#
# Everything else -- the light majority -- is registered here as ONE ctest
# entry per gtest SUITE, running `--gtest_filter=<Suite>.*` minus the same
# exclusions, so a suite of 60 one-second cases costs one start-up instead of
# sixty. Suites larger than CHUNK_SIZE cases are split into gtest shards
# (GTEST_TOTAL_SHARDS / GTEST_SHARD_INDEX in the entry's environment), so no
# single entry grows past a few minutes under a sanitizer and the width stays
# usable. Each entry's TIMEOUT scales with its case count, and its COST is its
# case count so ctest schedules the big suites first.
#
# What stays the same for a test author: cases still get their own TempDir()
# (keyed per case beneath a per-process root -- see
# docs/agent-rules/shared-temp-dir-test-isolation.md), the renderer-state and
# GL-error listeners still restore between cases, and the GTEST_OUTPUT XML the
# CI report reads is still one record per case. What changes: a crash in one
# case takes the rest of its suite's cases with it (that is why the crash-prone
# GPU and long cases keep their own processes), and process-global state a case
# leaks is now visible to the next case in the same suite -- which is the same
# contract a local `--gtest_filter=Suite.*` run has always had.
#
# Arguments (all via -D):
#   TEST_EXECUTABLE     the built test binary
#   TEST_WORKING_DIR    working directory for every entry (OloEditor/)
#   TEST_EXTRA_ARGS     ;-list of extra args for every entry (OLO_TESTS_CTEST_ARGS)
#   TEST_EXCLUDE_FILTER gtest patterns (':'-separated) that must NOT run here
#                       because they are registered per case elsewhere
#   CTEST_FILE          the file to (re)write
#   TEST_DISCOVERY_TIMEOUT  seconds allowed for the --gtest_list_tests run
#   CHUNK_SIZE          cases per entry before a suite is split into shards
#   DEFAULT_TIMEOUT     floor for each entry's TIMEOUT property (seconds)

cmake_minimum_required(VERSION 3.29)

foreach(required IN ITEMS TEST_EXECUTABLE TEST_WORKING_DIR TEST_EXCLUDE_FILTER CTEST_FILE)
	if(NOT DEFINED ${required})
		message(FATAL_ERROR "OloSuiteDiscovery: ${required} is required")
	endif()
endforeach()
if(NOT DEFINED TEST_DISCOVERY_TIMEOUT)
	set(TEST_DISCOVERY_TIMEOUT 120)
endif()
if(NOT DEFINED CHUNK_SIZE)
	set(CHUNK_SIZE 40)
endif()
if(NOT DEFINED DEFAULT_TIMEOUT)
	set(DEFAULT_TIMEOUT 600)
endif()

if(NOT EXISTS "${TEST_EXECUTABLE}")
	message(FATAL_ERROR "OloSuiteDiscovery: test executable does not exist: '${TEST_EXECUTABLE}'")
endif()

# The negative half of every entry's filter. gtest's grammar is
# `positive[-negative]`, so the exclusions ride on each suite's own positive
# pattern; passing the same string to the listing below gives exactly the set of
# cases these entries will run, and nothing the per-case discoveries also run.
set(exclude "${TEST_EXCLUDE_FILTER}")

execute_process(
	COMMAND "${TEST_EXECUTABLE}" --gtest_list_tests "--gtest_filter=*-${exclude}"
	WORKING_DIRECTORY "${TEST_WORKING_DIR}"
	TIMEOUT ${TEST_DISCOVERY_TIMEOUT}
	OUTPUT_VARIABLE listing
	RESULT_VARIABLE result
)
if(NOT result EQUAL 0)
	message(FATAL_ERROR
		"OloSuiteDiscovery: listing the tests failed (exit ${result}). The binary or the\n"
		"working directory is wrong, or the listing took longer than ${TEST_DISCOVERY_TIMEOUT} s.\n"
		"  Executable: '${TEST_EXECUTABLE}'\n"
		"  Working dir: '${TEST_WORKING_DIR}'")
endif()

# Parse gtest's listing. Suites are the lines that do not start with a space and
# end in '.' (after the "  # TypeParam = ..." comment is dropped); cases are the
# indented lines beneath them. The engine prints a few log lines before the
# listing ("[hh:mm:ss] OloEngine: ..."); they neither end in '.' nor start with
# a space, so the shape test drops them.
set(suites "")
set(current "")
string(REPLACE "\n" ";" lines "${listing}")
foreach(line IN LISTS lines)
	string(REGEX REPLACE "\r$" "" line "${line}")
	if(line STREQUAL "")
		continue()
	endif()
	string(REGEX REPLACE "^([^#]*[^ #])  *#.*$" "\\1" body "${line}")
	if(body MATCHES "^  ")
		if(NOT current STREQUAL "")
			math(EXPR count_${current_id} "${count_${current_id}} + 1")
		endif()
	elseif(body MATCHES "^([^ ].*)\\.$")
		set(current "${CMAKE_MATCH_1}")
		string(MAKE_C_IDENTIFIER "${current}" current_id)
		if(NOT DEFINED count_${current_id})
			set(count_${current_id} 0)
			set(name_${current_id} "${current}")
			list(APPEND suites "${current_id}")
		endif()
	else()
		set(current "")
	endif()
endforeach()

if(suites STREQUAL "")
	message(FATAL_ERROR
		"OloSuiteDiscovery: the listing produced no suites. A filter that matches nothing, or a\n"
		"binary whose gtest main did not run, both look like this; neither is a usable test set.")
endif()

set(script "")
set(entries 0)
set(cases 0)
foreach(id IN LISTS suites)
	set(suite "${name_${id}}")
	set(count "${count_${id}}")
	if(count EQUAL 0)
		continue()
	endif()
	math(EXPR cases "${cases} + ${count}")
	math(EXPR chunks "(${count} + ${CHUNK_SIZE} - 1) / ${CHUNK_SIZE}")
	math(EXPR per_chunk "(${count} + ${chunks} - 1) / ${chunks}")
	# Generous: a light case is ~1 s under ASan and ~2-3 s under TSan, and this is a
	# ceiling for a hang, not a budget.
	math(EXPR timeout "120 + 8 * ${per_chunk}")
	if(timeout LESS DEFAULT_TIMEOUT)
		set(timeout ${DEFAULT_TIMEOUT})
	endif()
	set(chunk 0)
	while(chunk LESS chunks)
		# The entry is named `<Suite>.*`, i.e. the gtest filter it runs, so every
		# `^Suite\.`-shaped --exclude-regex the CI jobs already carry keeps matching
		# it, and a chunk is `<Suite>.*[i/n]`. Suite names can hold '/' (type- and
		# value-parameterised suites); bracket-quote everything.
		if(chunks EQUAL 1)
			set(entry "${suite}.*")
		else()
			math(EXPR shown "${chunk} + 1")
			set(entry "${suite}.*[${shown}/${chunks}]")
		endif()
		set(command "add_test([==[${entry}]==] [==[${TEST_EXECUTABLE}]==] [==[--gtest_filter=${suite}.*-${exclude}]==]")
		foreach(arg IN LISTS TEST_EXTRA_ARGS)
			string(APPEND command " [==[${arg}]==]")
		endforeach()
		string(APPEND command ")\n")
		string(APPEND script "${command}")
		set(environment "")
		if(chunks GREATER 1)
			set(environment "ENVIRONMENT [==[GTEST_TOTAL_SHARDS=${chunks};GTEST_SHARD_INDEX=${chunk}]==]")
		endif()
		string(APPEND script
			"set_tests_properties([==[${entry}]==] PROPERTIES\n"
			"  WORKING_DIRECTORY [==[${TEST_WORKING_DIR}]==]\n"
			"  TIMEOUT ${timeout}\n"
			"  COST ${per_chunk}\n"
			"  LABELS [==[suite]==]\n"
			"  ${environment}\n"
			")\n")
		math(EXPR entries "${entries} + 1")
		math(EXPR chunk "${chunk} + 1")
	endwhile()
endforeach()

file(WRITE "${CTEST_FILE}" "# Generated by OloSuiteDiscovery.cmake; do not edit.\n${script}")
list(LENGTH suites suite_count)
message(STATUS "OloSuiteDiscovery: ${cases} cases in ${suite_count} suites -> ${entries} ctest entries (chunk ${CHUNK_SIZE})")
