#include "OloEnginePCH.h"
#include "OloEngine/Scripting/C#/ScriptEngine.h"
#include "OloEngine/Scripting/C#/ScriptGlue.h"
#include "OloEngine/Core/Application.h"
#include "OloEngine/Core/Buffer.h"
#include "OloEngine/Core/FileSystem.h"
#include "OloEngine/Core/Timer.h"

#include <mono/jit/jit.h>
#include <mono/metadata/assembly.h>
#include <mono/metadata/object.h>
#include "mono/metadata/tabledefs.h"
#include "mono/metadata/mono-debug.h"
#include "mono/metadata/threads.h"

#include "FileWatch.hpp"

namespace OloEngine
{

	static std::unordered_map<std::string, ScriptFieldType> s_ScriptFieldTypeMap =
	{
		{ "System.Single", ScriptFieldType::Float },
		{ "System.Double", ScriptFieldType::Double },
		{ "System.Boolean", ScriptFieldType::Bool },
		{ "System.Char", ScriptFieldType::Char },
		{ "System.Int16", ScriptFieldType::Short },
		{ "System.Int32", ScriptFieldType::Int },
		{ "System.Int64", ScriptFieldType::Long },
		{ "System.Byte", ScriptFieldType::Byte },
		{ "System.UInt16", ScriptFieldType::UShort },
		{ "System.UInt32", ScriptFieldType::UInt },
		{ "System.UInt64", ScriptFieldType::ULong },

		{ "OloEngine.Vector2", ScriptFieldType::Vector2 },
		{ "OloEngine.Vector3", ScriptFieldType::Vector3 },
		{ "OloEngine.Vector4", ScriptFieldType::Vector4 },

		{ "OloEngine.Entity", ScriptFieldType::Entity },
	};



	namespace Utils {

		static MonoAssembly* LoadMonoAssembly(const std::filesystem::path& assemblyPath, bool loadPDB = false)
		{
			ScopedBuffer fileData = FileSystem::ReadFileBinary(assemblyPath);

			// NOTE: We can't use this image for anything other than loading the assembly because this image doesn't have a reference to the assembly
			MonoImageOpenStatus status;
			MonoImage* image = ::mono_image_open_from_data_full(fileData.As<char>(), static_cast<u32>(fileData.Size()), 1, &status, 0);

			if (status != MONO_IMAGE_OK)
			{
				const char* errorMessage = ::mono_image_strerror(status);
				OLO_CORE_ERROR("[ScriptEngine] Error detected! {0}", errorMessage);
				return nullptr;
			}

			if (loadPDB)
			{
				std::filesystem::path pdbPath = assemblyPath;
				pdbPath.replace_extension(".pdb");

				if (std::filesystem::exists(pdbPath))
				{
					ScopedBuffer pdbFileData = FileSystem::ReadFileBinary(pdbPath);
					::mono_debug_open_image_from_memory(image, pdbFileData.As<const mono_byte>(), static_cast<int>(pdbFileData.Size()));
					OLO_CORE_INFO("Loaded PDB {}", pdbPath);
				}
			}

			std::string pathString = assemblyPath.string();
			MonoAssembly* assembly = ::mono_assembly_load_from_full(image, pathString.c_str(), &status, 0);
			::mono_image_close(image);

			return assembly;
		}

		void PrintAssemblyTypes(MonoAssembly* assembly)
		{
			MonoImage* image = ::mono_assembly_get_image(assembly);
			const MonoTableInfo* typeDefinitionsTable = ::mono_image_get_table_info(image, MONO_TABLE_TYPEDEF);
			i32 numTypes = ::mono_table_info_get_rows(typeDefinitionsTable);

			for (i32 i = 0; i < numTypes; ++i)
			{
				u32 cols[MONO_TYPEDEF_SIZE];
				::mono_metadata_decode_row(typeDefinitionsTable, i, cols, MONO_TYPEDEF_SIZE);

				const char* nameSpace = ::mono_metadata_string_heap(image, cols[MONO_TYPEDEF_NAMESPACE]);
				const char* name = ::mono_metadata_string_heap(image, cols[MONO_TYPEDEF_NAME]);
				OLO_CORE_TRACE("{}.{}", nameSpace, name);
			}
		}

		ScriptFieldType MonoTypeToScriptFieldType(MonoType* monoType)
		{
			std::string typeName = ::mono_type_get_name(monoType);

			if (!s_ScriptFieldTypeMap.contains(typeName))
			{
				OLO_CORE_ERROR("Unknown type: {}", typeName);
				return ScriptFieldType::None;
			}

			auto it = s_ScriptFieldTypeMap.find(typeName);
			return it->second;
		}
	}

	struct ScriptEngineData
	{
		MonoDomain* RootDomain = nullptr;
		MonoDomain* AppDomain = nullptr;

		MonoAssembly* CoreAssembly = nullptr;
		MonoImage* CoreAssemblyImage = nullptr;

		MonoAssembly* AppAssembly = nullptr;
		MonoImage* AppAssemblyImage = nullptr;

		std::filesystem::path CoreAssemblyFilepath;
		std::filesystem::path AppAssemblyFilepath;

		Ref<ScriptClass> EntityClass;

		std::unordered_map<std::string, Ref<ScriptClass>> EntityClasses;
		std::unordered_map<UUID, Ref<ScriptInstance>> EntityInstances;
		std::unordered_map<UUID, ScriptFieldMap> EntityScriptFields;

		Scope<filewatch::FileWatch<std::string>> AppAssemblyFileWatcher;
		bool AssemblyReloadPending = false;

		bool EnableDebugging = true;

		// Runtime
		Scene* SceneContext = nullptr;
	};

	static ScriptEngineData* s_Data = nullptr;

	static void OnAppAssemblyFileSystemEvent(const std::string_view, const filewatch::Event change_type)
	{
		if (!s_Data->AssemblyReloadPending && change_type == filewatch::Event::modified)
		{
			s_Data->AssemblyReloadPending = true;

			Application::Get().SubmitToMainThread([]()
			{
				s_Data->AppAssemblyFileWatcher.reset();
				ScriptEngine::ReloadAssembly();
			});
		}
	}

	void ScriptEngine::Init()
	{
		OLO_CORE_TRACE("[ScriptEngine] Initializing.");

		s_Data = new ScriptEngineData();

		InitMono();
		ScriptGlue::RegisterFunctions();

		if (bool status = LoadAssembly("Resources/Scripts/OloEngine-ScriptCore.dll"); !status)
		{
			OLO_CORE_ERROR("[ScriptEngine] Could not load OloEngine-ScriptCore assembly.");
			return;
		}

		if (bool status = LoadAppAssembly("SandboxProject/Assets/Scripts/Binaries/Sandbox-Scripting.dll"); !status)
		{
			OLO_CORE_ERROR("[ScriptEngine] Could not load app assembly.");
			return;
		}

		LoadAssemblyClasses();

		ScriptGlue::RegisterComponents();

		// Retrieve and instantiate class
		s_Data->EntityClass = Ref<ScriptClass>::Create("OloEngine", "Entity", true);
	}

	void ScriptEngine::Shutdown()
	{
		OLO_CORE_TRACE("[ScriptEngine] Shutting down.");
		ShutdownMono();
		delete s_Data;
	}

	void ScriptEngine::InitMono()
	{
		::mono_set_assemblies_path("mono/lib");

		if (s_Data->EnableDebugging)
		{
			std::string debuggerAgentArguments = "--debugger-agent=transport=dt_socket,address=127.0.0.1:2550,server=y,suspend=n,loglevel=3,logfile=MonoDebugger.log";

			// Enable mono soft debugger
			const char* options[2] = {
				debuggerAgentArguments.c_str(),
				"--soft-breakpoints"
			};

			::mono_jit_parse_options(2, (char**)options);
			::mono_debug_init(MONO_DEBUG_FORMAT_MONO);
		}

		MonoDomain* rootDomain = ::mono_jit_init("OloEngineJITRuntime");
		OLO_CORE_ASSERT(rootDomain, "Unable to initialize MONO JIT");

		// Store the root domain pointer
		s_Data->RootDomain = rootDomain;

		if (s_Data->EnableDebugging)
		{
			::mono_debug_domain_create(s_Data->RootDomain);
		}

		::mono_thread_set_main(::mono_thread_current());
	}

	void ScriptEngine::ShutdownMono()
	{
		::mono_domain_set(::mono_get_root_domain(), false);

		::mono_domain_unload(s_Data->AppDomain);
		s_Data->AppDomain = nullptr;

		::mono_jit_cleanup(s_Data->RootDomain);
		s_Data->RootDomain = nullptr;
	}

	bool ScriptEngine::LoadAssembly(const std::filesystem::path& filepath)
	{
		// Create an App Domain
		char domainName[] = "OloEngineScriptRuntime";
		s_Data->AppDomain = ::mono_domain_create_appdomain(domainName, nullptr);
		::mono_domain_set(s_Data->AppDomain, true);

		s_Data->CoreAssemblyFilepath = filepath;
		s_Data->CoreAssembly = Utils::LoadMonoAssembly(filepath, s_Data->EnableDebugging);
		if (s_Data->CoreAssembly == nullptr)
		{
			return false;
		}

		s_Data->CoreAssemblyImage = ::mono_assembly_get_image(s_Data->CoreAssembly);
		return true;
	}

	bool ScriptEngine::LoadAppAssembly(const std::filesystem::path& filepath)
	{
		s_Data->AppAssemblyFilepath = filepath;
		s_Data->AppAssembly = Utils::LoadMonoAssembly(filepath, s_Data->EnableDebugging);
		if (s_Data->AppAssembly == nullptr)
		{
			return false;
		}

		s_Data->AppAssemblyImage = ::mono_assembly_get_image(s_Data->AppAssembly);

		s_Data->AppAssemblyFileWatcher = CreateScope<filewatch::FileWatch<std::string>>(filepath.string(), OnAppAssemblyFileSystemEvent);
		s_Data->AssemblyReloadPending = false;
		return true;
	}

	void ScriptEngine::ReloadAssembly()
	{
		::mono_domain_set(::mono_get_root_domain(), false);

		::mono_domain_unload(s_Data->AppDomain);

		LoadAssembly(s_Data->CoreAssemblyFilepath);
		LoadAppAssembly(s_Data->AppAssemblyFilepath);
		LoadAssemblyClasses();

		ScriptGlue::RegisterComponents();

		// Retrieve and instantiate class
		s_Data->EntityClass = Ref<ScriptClass>::Create("OloEngine", "Entity", true);
	}

	void ScriptEngine::OnRuntimeStart(Scene* scene)
	{
		s_Data->SceneContext = scene;
	}

	bool ScriptEngine::EntityClassExists(const std::string& fullClassName)
	{
		return s_Data->EntityClasses.contains(fullClassName);
	}

	void ScriptEngine::OnCreateEntity(Entity entity)
	{
		const auto& sc = entity.GetComponent<ScriptComponent>();
		if (ScriptEngine::EntityClassExists(sc.ClassName))
		{
			UUID entityID = entity.GetUUID();

			Ref<ScriptInstance> instance = Ref<ScriptInstance>::Create(s_Data->EntityClasses[sc.ClassName], entity);
			s_Data->EntityInstances[entityID] = instance;

			// Copy field values
			if (s_Data->EntityScriptFields.contains(entityID))
			{
				const ScriptFieldMap& fieldMap = s_Data->EntityScriptFields.at(entityID);
				for (const auto& [name, fieldInstance] : fieldMap)
				{
					instance->SetFieldValueInternal(name, fieldInstance.m_Buffer);
				}
			}

			instance->InvokeOnCreate();
		}
	}

	void ScriptEngine::OnUpdateEntity(Entity entity, Timestep ts)
	{
		UUID entityUUID = entity.GetUUID();

		if (s_Data->EntityInstances.contains(entityUUID))
		{
			Ref<ScriptInstance> instance = s_Data->EntityInstances[entityUUID];
			instance->InvokeOnUpdate(static_cast<f32>(ts));
		}
		else
		{
			OLO_CORE_ERROR("Could not find ScriptInstance for entity {}", entityUUID);
		}
	}

	Scene* ScriptEngine::GetSceneContext()
	{
		return s_Data->SceneContext;
	}

	Ref<ScriptInstance> ScriptEngine::GetEntityScriptInstance(UUID entityID)
	{
		if (!s_Data->EntityInstances.contains(entityID))
		{
			return nullptr;
		}

		auto it = s_Data->EntityInstances.find(entityID);
		return it->second;
	}

	Ref<ScriptClass> ScriptEngine::GetEntityClass(const std::string& name)
	{
		if (!s_Data->EntityClasses.contains(name))
		{
			return nullptr;
		}

		return s_Data->EntityClasses.at(name);
	}

	void ScriptEngine::OnRuntimeStop()
	{
		s_Data->SceneContext = nullptr;

		s_Data->EntityInstances.clear();
	}

	ScriptFieldMap& ScriptEngine::GetScriptFieldMap(Entity entity)
	{
		OLO_CORE_ASSERT(entity);

		UUID entityID = entity.GetUUID();
		return s_Data->EntityScriptFields[entityID];
	}

	std::unordered_map<std::string, Ref<ScriptClass>> ScriptEngine::GetEntityClasses()
	{
		return s_Data->EntityClasses;
	}

	void ScriptEngine::LoadAssemblyClasses()
	{
		s_Data->EntityClasses.clear();

		const MonoTableInfo* typeDefinitionsTable = ::mono_image_get_table_info(s_Data->AppAssemblyImage, MONO_TABLE_TYPEDEF);
		i32 numTypes = ::mono_table_info_get_rows(typeDefinitionsTable);
		MonoClass* entityClass = ::mono_class_from_name(s_Data->CoreAssemblyImage, "OloEngine", "Entity");

		for (i32 i = 0; i < numTypes; ++i)
		{
			u32 cols[MONO_TYPEDEF_SIZE];
			::mono_metadata_decode_row(typeDefinitionsTable, i, cols, MONO_TYPEDEF_SIZE);

			const char* nameSpace = ::mono_metadata_string_heap(s_Data->AppAssemblyImage, cols[MONO_TYPEDEF_NAMESPACE]);
			const char* className = ::mono_metadata_string_heap(s_Data->AppAssemblyImage, cols[MONO_TYPEDEF_NAME]);

			std::string fullName;
			if (std::strlen(nameSpace) != 0)
			{
				fullName = fmt::format("{}.{}", nameSpace, className);
			}
			else
			{
				fullName = className;
			}

			MonoClass* monoClass = ::mono_class_from_name(s_Data->AppAssemblyImage, nameSpace, className);
			if (monoClass == entityClass)
			{
				continue;
			}

			if (bool isEntity = ::mono_class_is_subclass_of(monoClass, entityClass, false); !isEntity)
				continue;

			Ref<ScriptClass> scriptClass = Ref<ScriptClass>::Create(nameSpace, className);
			s_Data->EntityClasses[fullName] = scriptClass;


			// This routine is an iterator routine for retrieving the fields in a class.
			// You must pass a gpointer that points to zero and is treated as an opaque handle
			// to iterate over all of the elements. When no more values are available, the return value is NULL.

			int fieldCount = ::mono_class_num_fields(monoClass);
			OLO_CORE_WARN("{} has {} fields:", className, fieldCount);
			void* iterator = nullptr;
			while (MonoClassField* field = ::mono_class_get_fields(monoClass, &iterator))
			{
				const char* fieldName = ::mono_field_get_name(field);
				u32 flags = ::mono_field_get_flags(field);
				if (flags & FIELD_ATTRIBUTE_PUBLIC)
				{
					MonoType* type = ::mono_field_get_type(field);
					ScriptFieldType fieldType = Utils::MonoTypeToScriptFieldType(type);
					OLO_CORE_WARN("  {} ({})", fieldName, Utils::ScriptFieldTypeToString(fieldType));

					scriptClass->m_Fields[fieldName] = { fieldType, fieldName, field };
				}
			}
		}

		// TODO(olbu): Find out why Cherno has that line below, seems unnecessary
		// auto const& entityClasses = s_Data->EntityClasses;
		//mono_field_get_value()
	}

	MonoImage* ScriptEngine::GetCoreAssemblyImage()
	{
		return s_Data->CoreAssemblyImage;
	}

	MonoObject* ScriptEngine::GetManagedInstance(UUID uuid)
	{
		OLO_CORE_ASSERT(s_Data->EntityInstances.contains(uuid));
		return s_Data->EntityInstances.at(uuid)->GetManagedObject();
	}

	MonoString* ScriptEngine::CreateString(const char* string)
	{
		return ::mono_string_new(s_Data->AppDomain, string);
	}

	MonoObject* ScriptEngine::InstantiateClass(MonoClass* monoClass)
	{
		MonoObject* instance = ::mono_object_new(s_Data->AppDomain, monoClass);
		::mono_runtime_object_init(instance);
		return instance;
	}

	ScriptClass::ScriptClass(const std::string& classNamespace, const std::string& className, bool isCore)
		: m_ClassNamespace(classNamespace), m_ClassName(className)
	{
		m_MonoClass = ::mono_class_from_name(isCore ? s_Data->CoreAssemblyImage : s_Data->AppAssemblyImage, classNamespace.c_str(), className.c_str());
	}

	MonoObject* ScriptClass::Instantiate()
	{
		return ScriptEngine::InstantiateClass(m_MonoClass);
	}

	MonoMethod* ScriptClass::GetMethod(const std::string& name, int parameterCount)
	{
		return ::mono_class_get_method_from_name(m_MonoClass, name.c_str(), parameterCount);
	}

	MonoObject* ScriptClass::InvokeMethod(MonoObject* instance, MonoMethod* method, void** params)
	{
		MonoObject* exception = nullptr;
		return ::mono_runtime_invoke(method, instance, params, &exception);
	}

	ScriptInstance::ScriptInstance(const Ref<ScriptClass>& scriptClass, Entity entity)
		: m_ScriptClass(scriptClass)
	{
		m_Instance = const_cast<ScriptClass*>(scriptClass.Raw())->Instantiate();

		m_Constructor = s_Data->EntityClass->GetMethod(".ctor", 1);
		m_OnCreateMethod = const_cast<ScriptClass*>(scriptClass.Raw())->GetMethod("OnCreate", 0);
		m_OnUpdateMethod = const_cast<ScriptClass*>(scriptClass.Raw())->GetMethod("OnUpdate", 1);

		// Call Entity constructor
		{
			UUID entityID = entity.GetUUID();
			void* param = &entityID;
			m_ScriptClass->InvokeMethod(m_Instance, m_Constructor, &param);
		}
	}

	void ScriptInstance::InvokeOnCreate()
	{
		if (m_OnCreateMethod)
		{
			m_ScriptClass->InvokeMethod(m_Instance, m_OnCreateMethod);
		}
	}

	void ScriptInstance::InvokeOnUpdate(f32 ts)
	{
		if (m_OnUpdateMethod)
		{
			void* param = &ts;
			m_ScriptClass->InvokeMethod(m_Instance, m_OnUpdateMethod, &param);
		}
	}

	bool ScriptInstance::GetFieldValueInternal(const std::string& name, void* buffer)
	{
		const auto& fields = m_ScriptClass->GetFields();
		if (!fields.contains(name))
		{
			return false;
		}

		auto it = fields.find(name);
		const ScriptField& field = it->second;
		::mono_field_get_value(m_Instance, field.ClassField, buffer);
		return true;
	}

	bool ScriptInstance::SetFieldValueInternal(const std::string& name, const void* value)
	{
		const auto& fields = m_ScriptClass->GetFields();
		if (!fields.contains(name))
		{
			return false;
		}

		auto it = fields.find(name);
		const ScriptField& field = it->second;
		::mono_field_set_value(m_Instance, field.ClassField, (void*)value);
		return true;
	}
}
