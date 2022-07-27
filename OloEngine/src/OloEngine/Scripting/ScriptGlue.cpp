#include "OloEnginePCH.h"
#include "ScriptGlue.h"

#include "mono/metadata/object.h"

namespace OloEngine {

#define OLO_ADD_INTERNAL_CALL(Name) mono_add_internal_call("OloEngine.InternalCalls::" #Name, Name)

	static void NativeLog(MonoString* string, int parameter)
	{
		char* cStr = mono_string_to_utf8(string);
		std::string str(cStr);
		mono_free(cStr);
		std::cout << str << ", " << parameter << std::endl;
	}

	static void NativeLog_Vector(glm::vec3* parameter, glm::vec3* outResult)
	{
		//TODO(olbu): Fix the logger, glm::vec3* is not valid type, need to provide a formatter<T> specialization
		//https://fmt.dev/latest/api.html#udt
		//OLO_CORE_WARN("Value: {0}", *parameter);
		*outResult = glm::normalize(*parameter);
	}

	static float NativeLog_VectorDot(glm::vec3* parameter)
	{
		//TODO(olbu): Fix the logger, glm::vec3* is not valid type, need to provide a formatter<T> specialization
		//https://fmt.dev/latest/api.html#udt
		//OLO_CORE_WARN("Value: {0}", *parameter);
		return glm::dot(*parameter, *parameter);
	}

	void ScriptGlue::RegisterFunctions()
	{
		OLO_ADD_INTERNAL_CALL(NativeLog);
		OLO_ADD_INTERNAL_CALL(NativeLog_Vector);
		OLO_ADD_INTERNAL_CALL(NativeLog_VectorDot);
	}

}
