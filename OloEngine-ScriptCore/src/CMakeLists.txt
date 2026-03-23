set(SOURCES
		"OloEngine/Camera.cs"
		"OloEngine/Debug.cs"
		"OloEngine/GameApplication.cs"
		"OloEngine/GamepadCodes.cs"
		"OloEngine/Input.cs"
		"OloEngine/InternalCalls.cs"
		"OloEngine/ISaveable.cs"
		"OloEngine/KeyCode.cs"
		"OloEngine/NetworkManager.cs"
		"OloEngine/Physics.cs"
		"OloEngine/SaveGame.cs"
		"OloEngine/Vector2.cs"
		"OloEngine/Vector3.cs"
		"OloEngine/Vector4.cs"
		
		"OloEngine/Scene/Components.cs"
		"OloEngine/Scene/Entity.cs"
)

add_library(OloEngine-ScriptCore SHARED
		${SOURCES}
)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}
			FILES ${SOURCES}
)
