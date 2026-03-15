namespace OloEngine
{
	/// <summary>
	/// Interface for scripts that want custom save/load behavior.
	/// Implement OnSave() to serialize custom fields and OnLoad() to restore them.
	/// </summary>
	public interface ISaveable
	{
		/// <summary>
		/// Called during save. Return a byte array containing your custom save data.
		/// Return null or empty array if there's nothing to save.
		/// </summary>
		byte[] OnSave();

		/// <summary>
		/// Called during load. Restore your custom state from the provided byte array.
		/// The data parameter will be whatever was returned from OnSave().
		/// </summary>
		void OnLoad(byte[] data);
	}
}
