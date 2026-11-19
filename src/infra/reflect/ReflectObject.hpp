#pragma once

#include <infra/reflect/TypeInfo.hpp>
#include <typeinfo>
#include <utility>

namespace infra
{

class ReflectObject
{
public:
    ReflectObject() noexcept = default;

    template<typename Ty>
        requires (!std::is_same_v<std::decay_t<Ty>, ReflectObject>)
    ReflectObject(Ty &&Value)
    {
        using U = std::decay_t<Ty>;
        Ptr = new U(std::forward<Ty>(Value));
        Id = &TypeInfo<U>::TypeId;
        Vtable = &VTable<U>::get();
    }

    ReflectObject(const ReflectObject &Other)
    {
        if (Other.Ptr) { Ptr = Other.Vtable->clone(Other.Ptr); Id = Other.Id; Vtable = Other.Vtable; }
    }
    ReflectObject(ReflectObject &&Other) noexcept
    {
        Ptr          = Other.Ptr; 
        Id           = Other.Id; 
        Vtable       = Other.Vtable;
        Other.Ptr    = nullptr; 
        Other.Id     = nullptr; 
        Other.Vtable = nullptr;
    }
    ReflectObject &operator=(const ReflectObject &Other)
    {
        if (this != &Other)
        {
            reset();
            if (Other.Ptr) 
            { 
                Ptr    = Other.Vtable->clone(Other.Ptr); 
                Id     = Other.Id; 
                Vtable = Other.Vtable; 
            }
        }
        return *this;
    }
    ReflectObject &operator=(ReflectObject &&Other) noexcept
    {
        if (this != &Other) 
        { 
            reset(); 
            Ptr          = Other.Ptr; 
            Id           = Other.Id; 
            Vtable       = Other.Vtable; 
            Other.Ptr    = nullptr; 
            Other.Id     = nullptr; 
            Other.Vtable = nullptr; 
        }
        return *this;
    }
    ~ReflectObject() { reset(); }

    [[nodiscard]] bool hasValue() const noexcept { return Ptr != nullptr; }

    template<typename Ty>
    [[nodiscard]] bool is() const noexcept
    {
        return Ptr != nullptr && Id == &TypeInfo<std::decay_t<Ty>>::TypeId;
    }

    template<typename Ty> Ty &cast()
    {
        using U = std::decay_t<Ty>;
        if (!is<U>()) 
        {
            throw std::bad_cast();
        }
        return *static_cast<U *>(Ptr);
    }
    template<typename Ty> const Ty &cast() const
    {
        using U = std::decay_t<Ty>;
        if (!is<U>()) 
        {
            throw std::bad_cast();
        }
        return *static_cast<const U *>(Ptr);
    }

private:
    struct VTableBase { void *(*clone)(const void *); void (*destroy)(void *); };
    template<typename Ty>
    struct VTable
    {
        static void *clone(const void *p) { return new Ty(*static_cast<const Ty *>(p)); }
        static void destroy(void *p) { delete static_cast<Ty *>(p); }
        static const VTableBase &get() 
        { 
            static const VTableBase Vt{ &clone, &destroy }; 
            return Vt;
        }
    };
    void reset() noexcept
    {
        if (Ptr) 
        {
            Vtable->destroy(Ptr);
        }
        Ptr    = nullptr; 
        Id     = nullptr; 
        Vtable = nullptr;
    }
    void *Ptr { nullptr };
    const void *Id { nullptr };
    const VTableBase *Vtable { nullptr };
};


}
