#helper macro to assign a boolean variable 
macro(SET_BOOL var)
     if(${ARGN})
         set(${var} ON)
     else()
         set(${var} OFF)
     endif()
endmacro()

option(FETCH_AUTO      "automaticly download and build dependancies" OFF)

# here we have to do some special logic to determine if we should
# automaticly download sparsehash. This is done if we used  
#
# does not define FETCH_SPARSEHASH and define FETCH_AUTO 
# or
# define FETCH_SPARSEHASH as True/ON
SET_BOOL(FETCH_SPARSEHASH_AUTO 
	(DEFINED FETCH_SPARSEHASH AND FETCH_SPARSEHASH) OR
	((NOT DEFINED FETCH_SPARSEHASH) AND (FETCH_AUTO)))
    
# here we have to do some special logic to determine if we should
# automaticly download sparsehash. This is done if we used  
#
# does not define FETCH_LIBOTE and define FETCH_AUTO 
# or
# define FETCH_LIBOTE as True/ON
SET_BOOL(FETCH_LIBOTE_AUTO 
	(DEFINED FETCH_LIBOTE AND FETCH_LIBOTE) OR
	((NOT DEFINED FETCH_LIBOTE) AND (FETCH_AUTO)))


message(STATUS "projTemp options\n=======================================================")

message(STATUS "Option: FETCH_AUTO            = ${FETCH_AUTO}")
message(STATUS "Option: FETCH_SPARSEHASH      = ${FETCH_SPARSEHASH}")
message(STATUS "Option: FETCH_LIBOTE          = ${FETCH_LIBOTE}\n")



# here we define any compile time options we want to set. 
option(PROJTEMP_ENABLE_X      "compile the library with feature X" ON)


message(STATUS "Option: PROJTEMP_ENABLE_X     = ${PROJTEMP_ENABLE_X}\n")
© 2022 GitHub, Inc.
Terms
Privacy
Security
Status
Docs