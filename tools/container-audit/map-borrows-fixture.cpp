// OLO_TEST_LAYER: unit
// Input for clang-query, not a linked test translation unit.
// "Unsafe" means unsafe after a relocatable UE-map replacement. std::unordered_map
// preserves element references across insertion; these are not existing bugs.
#include <unordered_map>

int SafeLocalBorrow(std::unordered_map<int, int>& values)
{
    auto& value = values[0];
    return ++value;
}

int UnsafeAcrossInsert(std::unordered_map<int, int>& values)
{
    auto& value = values[0];
    values.emplace(1, 2);
    return ++value;
}

// Deliberate negative controls: real patterns found in the target subsystems.
// These invalidate a claim that a local lexical matcher permits TMap migration.
int* EscapingBorrow(std::unordered_map<int, int>& values)
{
    return &values.find(0)->second;
}

void StoreBorrow(std::unordered_map<int, int>& values, int*& output)
{
    output = &values.find(0)->second;
}

void InsertThroughHelper(std::unordered_map<int, int>& values);
int UnsafeAcrossHelper(std::unordered_map<int, int>& values)
{
    auto& value = values[0];
    InsertThroughHelper(values);
    return ++value;
}

int SafeMutationBeforeBorrow(std::unordered_map<int, int>& values)
{
    values.emplace(1, 2);
    auto& value = values[0];
    return ++value;
}

int SafeOtherMapMutation(std::unordered_map<int, int>& values,
                         std::unordered_map<int, int>& other)
{
    auto& value = values[0];
    other.emplace(1, 2);
    return ++value;
}
