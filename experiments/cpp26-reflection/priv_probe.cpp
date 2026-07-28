// Can reflection READ a private member (obtained via unchecked access context)
// by splicing it? OloHeaderTool cannot — `comp.m_Private` won't compile.
#include <meta>
#include <string_view>
#include <cstdio>

namespace sm = std::meta;

struct TransformLike {
private:
    int m_Rotation = 42;          // private — OloHeaderTool would fail on this
    float m_RotationEuler = 1.5f; // private
public:
    int m_Translation = 7;        // public
};

template <typename T>
void DumpAll(const T& obj) {
    // unchecked() enumerates ALL members incl. private
    template for (constexpr auto m :
                  std::define_static_array(sm::nonstatic_data_members_of(^^T, sm::access_context::unchecked())))
    {
        constexpr std::string_view name = sm::identifier_of(m);
        auto const& v = obj.[:m:];                    // splice-access — private too?
        std::printf("  %.*s = %g\n", (int)name.size(), name.data(), (double)v);
    }
}

int main() {
    TransformLike t;
    std::puts("all members (incl. private) via reflection:");
    DumpAll(t);
    return 0;
}
