set(SOURCES
		"Camera.cs"
		"CameraFollow.cs"
		"DamageFlashHelper.cs"
		"FireMageAI.cs"
		"GameManager.cs"
		"GamepadPlayer.cs"
		"GoblinAI.cs"
		"Player.cs"
		"PlayerController.cs"
)

add_library("Sandbox-Scripting" SHARED
		${SOURCES}
)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}
			FILES ${SOURCES}
)
