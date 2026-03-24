#pragma once
#include <concepts>

template<class T>
concept WrapperCompatile =
std::is_default_constructible_v<T> &&
std::is_copy_constructible_v<T> &&
std::is_copy_assignable_v<T> &&
std::is_move_constructible_v<T> &&
std::is_move_assignable_v<T>;

template <WrapperCompatile T>
struct TypeWrapper
{
    TypeWrapper() = default;

    // Copy constructors
    TypeWrapper(const TypeWrapper& other) : m_obj(other.m_obj)
    {}

    TypeWrapper(const T& obj) : m_obj(obj)
    {}

    TypeWrapper(T&& obj) : m_obj(obj)
    {}

    TypeWrapper(TypeWrapper&&) = default;

    // Assignment operators
    TypeWrapper& operator=(T& obj)
    {
        m_obj = obj;
        return *this;
    }

    TypeWrapper& operator=(TypeWrapper& other)
    {
        m_obj = other.m_obj;
        return *this;
    }

    TypeWrapper& operator=(T&& obj)
    {
        m_obj = obj;
        return *this;
    }

    TypeWrapper& operator=(TypeWrapper&&) = default;

    // Parmeterized constructor
    template <class ...Args>
    TypeWrapper(const Args&... args) :
        //m_obj(std::forward<const Args&>(args)...)
        m_obj(args...)
    {}

    T* operator->()
    {
        return &m_obj;
    }

    T const* operator->() const
    {
        return &m_obj;
    }

    T& operator*()
    {
        return m_obj;
    }

    T const& operator*() const
    {
        return m_obj;
    }

    private:
    T m_obj;
};