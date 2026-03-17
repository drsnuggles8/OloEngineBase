namespace OloEngine
{
	public enum GamepadButton : byte
	{
		South = 0,       // A (Xbox) / Cross (PS)
		East = 1,        // B (Xbox) / Circle (PS)
		West = 2,        // X (Xbox) / Square (PS)
		North = 3,       // Y (Xbox) / Triangle (PS)
		LeftBumper = 4,
		RightBumper = 5,
		Back = 6,        // Select / Share
		Start = 7,       // Menu / Options
		Guide = 8,       // Xbox / PS button
		LeftThumb = 9,   // L3
		RightThumb = 10, // R3
		DPadUp = 11,
		DPadRight = 12,
		DPadDown = 13,
		DPadLeft = 14
	}

	public enum GamepadAxis : byte
	{
		LeftX = 0,
		LeftY = 1,
		RightX = 2,
		RightY = 3,
		LeftTrigger = 4,
		RightTrigger = 5
	}
}
