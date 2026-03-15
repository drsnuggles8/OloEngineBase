namespace OloEngine
{
	public enum SaveLoadResult
	{
		Success = 0,
		FileNotFound,
		CorruptedFile,
		ChecksumMismatch,
		DecompressionFailed,
		SerializationFailed,
		NoActiveScene,
		IOError,
		InvalidInput
	}

	public static class SaveGame
	{
		public static SaveLoadResult Save(string slotName, string displayName = null)
		{
			if (string.IsNullOrEmpty(slotName))
				throw new System.ArgumentException("slotName cannot be null or empty", nameof(slotName));
			return (SaveLoadResult)InternalCalls.SaveGame_Save(slotName, displayName ?? slotName);
		}

		public static SaveLoadResult Load(string slotName)
		{
			if (string.IsNullOrEmpty(slotName))
				throw new System.ArgumentException("slotName cannot be null or empty", nameof(slotName));
			return (SaveLoadResult)InternalCalls.SaveGame_Load(slotName);
		}

		public static SaveLoadResult QuickSave()
			=> (SaveLoadResult)InternalCalls.SaveGame_QuickSave();

		public static SaveLoadResult QuickLoad()
			=> (SaveLoadResult)InternalCalls.SaveGame_QuickLoad();

		public static bool DeleteSave(string slotName)
		{
			if (string.IsNullOrEmpty(slotName))
				throw new System.ArgumentException("slotName cannot be null or empty", nameof(slotName));
			return InternalCalls.SaveGame_DeleteSave(slotName);
		}

		public static bool ValidateSave(string slotName)
		{
			if (string.IsNullOrEmpty(slotName))
				throw new System.ArgumentException("slotName cannot be null or empty", nameof(slotName));
			return InternalCalls.SaveGame_ValidateSave(slotName);
		}
	}
}
