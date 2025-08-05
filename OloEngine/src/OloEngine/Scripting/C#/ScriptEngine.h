#pragma once
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"

#include <filesystem>
#include <string>
#include <map>

extern "C" {
	using MonoClass = struct _MonoClass;
	using MonoObject = struct _MonoObject;
	using MonoMethod = struct _MonoMethod;
	using MonoAssembly = struct _MonoAssembly;
	using MonoImage = struct _MonoImage;
	using MonoClassField = struct _MonoClassField;
	using MonoString =  struct _MonoString;
}

namespace OloEngine
{

	enum class ScriptFieldType
	{
		None = 0,
		Float, Double,
		Bool, Char, Byte, Short, Int, Long,
		UByte, UShort, UInt, ULong,
		Vector2, Vector3, Vector4,
		Entity
	};

	struct ScriptField
	{
		ScriptFieldType Type;
		std::string Name;

		MonoClassField* ClassField;
	};

	struct ScriptFieldInstance
	{
		ScriptField Field;

		ScriptFieldInstance()
		{
			::memset(m_Buffer, 0, sizeof(m_Buffer));
		}

		template<typename T>
		T GetValue()
		{
			static_assert(sizeof(T) <= 16, "Type too large!");
			return *(T*)m_Buffer;
		}

		template<typename T>
		void SetValue(T value)
		{
			static_assert(sizeof(T) <= 16, "Type too large!");
			::memcpy(m_Buffer, &value, sizeof(T));
		}
	private:
		u8 m_Buffer[16];

		friend class ScriptEngine;
		friend class ScriptInstance;
	};

	using ScriptFieldMap = std::unordered_map<std::string, ScriptFieldInstance>;

	class ScriptClass : public RefCounted
	{
	public:
		ScriptClass() = default;
		ScriptClass(const std::string& classNamespace, const std::string& className, bool isCore = false);

		MonoObject* Instantiate() const;
		MonoMethod* GetMethod(const std::string& name, int parameterCount) const;
		MonoObject* InvokeMethod(MonoObject* instance, MonoMethod* method, void** params = nullptr);

        [[nodiscard("Store this!")]] const std::map<std::string, ScriptField>& GetFields() const { return m_Fields; }
	private:
		std::string m_ClassNamespace;
		std::string m_ClassName;

		std::map<std::string, ScriptField> m_Fields;

		MonoClass* m_MonoClass = nullptr;

		friend class ScriptEngine;
	};

	class ScriptInstance : public RefCounted
	{
	public:
		ScriptInstance(const Ref<ScriptClass>& scriptClass, Entity entity);

		void InvokeOnCreate();
		void InvokeOnUpdate(f32 ts);

        [[nodiscard("Store this!")]] Ref<ScriptClass> GetScriptClass() const { return m_ScriptClass; }

		template<typename T>
		T GetFieldValue(const std::string& name)
		{
			static_assert(sizeof(T) <= 16, "Type too large!");

			if (bool success = GetFieldValueInternal(name, s_FieldValueBuffer); !success)
			{
				return T();
			}

			return *(T*)s_FieldValueBuffer;
		}

		template<typename T>
		void SetFieldValue(const std::string& name, T value)
		{
			static_assert(sizeof(T) <= 16, "Type too large!");

			SetFieldValueInternal(name, &value);
		}

		[[nodiscard("Store this!")]] MonoObject * GetManagedObject() { return m_Instance; }
	private:
		bool GetFieldValueInternal(const std::string& name, void* buffer);
		bool SetFieldValueInternal(const std::string& name, const void* value);
	private:
		Ref<ScriptClass> m_ScriptClass;

		MonoObject* m_Instance = nullptr;
		MonoMethod* m_Constructor = nullptr;
		MonoMethod* m_OnCreateMethod = nullptr;
		MonoMethod* m_OnUpdateMethod = nullptr;

		inline static char s_FieldValueBuffer[16];

		friend class ScriptEngine;
		friend struct ScriptFieldInstance;
	};

	class ScriptEngine
	{
	public:
		static void Init();
		static void Shutdown();

		static bool LoadAssembly(const std::filesystem::path& filepath);
		static bool LoadAppAssembly(const std::filesystem::path& filepath);

		static void ReloadAssembly();

		static void OnRuntimeStart(Scene* scene);
		static void OnRuntimeStop();

		static bool EntityClassExists(const std::string& fullClassName);
		static void OnCreateEntity(Entity entity);
		static void OnUpdateEntity(Entity entity, Timestep ts);

		[[nodiscard("Store this!")]] static Scene* GetSceneContext();
		[[nodiscard("Store this!")]] static Ref<ScriptInstance> GetEntityScriptInstance(UUID entityID);
		[[nodiscard("Store this!")]] static Ref<ScriptClass> GetEntityClass(const std::string& name);
		[[nodiscard("Store this!")]] static std::unordered_map<std::string, Ref<ScriptClass>> GetEntityClasses();
		[[nodiscard("Store this!")]] static ScriptFieldMap& GetScriptFieldMap(Entity entity);

		[[nodiscard("Store this!")]] static MonoImage* GetCoreAssemblyImage();

		[[nodiscard("Store this!")]] static MonoObject* GetManagedInstance(UUID uuid);

		static MonoString* CreateString(const char* string);
	private:
		static void InitMono();
		static void ShutdownMono();

		static MonoObject* InstantiateClass(MonoClass* monoClass);
		static void LoadAssemblyClasses();

		friend class ScriptClass;
		friend class ScriptGlue;
	};

	namespace Utils {

		inline const char* ScriptFieldTypeToString(ScriptFieldType fieldType)
		{
			switch (fieldType)
			{
				using enum OloEngine::ScriptFieldType;
				case None:    return "None";
				case Float:   return "Float";
				case Double:  return "Double";
				case Bool:    return "Bool";
				case Char:    return "Char";
				case Byte:    return "Byte";
				case Short:   return "Short";
				case Int:     return "Int";
				case Long:    return "Long";
				case UByte:   return "UByte";
				case UShort:  return "UShort";
				case UInt:    return "UInt";
				case ULong:   return "ULong";
				case Vector2: return "Vector2";
				case Vector3: return "Vector3";
				case Vector4: return "Vector4";
				case Entity:  return "Entity";
			}
			OLO_CORE_ASSERT(false, "Unknown ScriptFieldType");
			return "None";
		}

		inline ScriptFieldType ScriptFieldTypeFromString(std::string_view fieldType)
		{
			using enum OloEngine::ScriptFieldType;
			if (fieldType == "None")    return None;
			if (fieldType == "Float")   return Float;
			if (fieldType == "Double")  return Double;
			if (fieldType == "Bool")    return Bool;
			if (fieldType == "Char")    return Char;
			if (fieldType == "Byte")    return Byte;
			if (fieldType == "Short")   return Short;
			if (fieldType == "Int")     return Int;
			if (fieldType == "Long")    return Long;
			if (fieldType == "UByte")   return UByte;
			if (fieldType == "UShort")  return UShort;
			if (fieldType == "UInt")    return UInt;
			if (fieldType == "ULong")   return ULong;
			if (fieldType == "Vector2") return Vector2;
			if (fieldType == "Vector3") return Vector3;
			if (fieldType == "Vector4") return Vector4;
			if (fieldType == "Entity")  return Entity;

			OLO_CORE_ASSERT(false, "Unknown ScriptFieldType");
			return None;
		}
	}
}
