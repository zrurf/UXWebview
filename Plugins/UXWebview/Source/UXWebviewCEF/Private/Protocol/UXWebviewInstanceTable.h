#pragma once

#include "CoreMinimal.h"

/**
 * Assigns webview instance ids and owns the matching record objects.
 * Header-only so the routing logic can be tested without the host.
 */
template <typename T>
class TUXWebviewInstanceTable
{
public:
	TUXWebviewInstanceTable() = default;

	~TUXWebviewInstanceTable()
	{
		Clear();
	}

	TUXWebviewInstanceTable(const TUXWebviewInstanceTable&) = delete;
	TUXWebviewInstanceTable& operator=(const TUXWebviewInstanceTable&) = delete;

	/** Takes ownership of Instance and returns its new id; 0 when null. */
	uint32 Add(T* Instance)
	{
		if (!Instance)
		{
			return 0;
		}

		const uint32 Id = NextId++;
		Instances.Add(Id, Instance);
		return Id;
	}

	T* Find(uint32 Id) const
	{
		T* const* Found = Instances.Find(Id);
		return Found ? *Found : nullptr;
	}

	bool Contains(uint32 Id) const
	{
		return Instances.Contains(Id);
	}

	/** Destroys the record; returns false when the id is unknown. */
	bool Remove(uint32 Id)
	{
		T** Found = Instances.Find(Id);
		if (!Found)
		{
			return false;
		}

		T* Instance = *Found;
		Instances.Remove(Id);
		delete Instance;
		return true;
	}

	void Clear()
	{
		for (const TPair<uint32, T*>& Pair : Instances)
		{
			delete Pair.Value;
		}
		Instances.Reset();
	}

	int32 Num() const
	{
		return Instances.Num();
	}

	uint32 PeekNextId() const
	{
		return NextId;
	}

	/** Visits every record; the callback receives (id, record). */
	template <typename FnType>
	void ForEach(FnType&& Fn) const
	{
		for (const TPair<uint32, T*>& Pair : Instances)
		{
			Fn(Pair.Key, *Pair.Value);
		}
	}

private:
	TMap<uint32, T*> Instances;
	uint32 NextId = 1;
};
