#ifndef CRAMION_CORE_SHARED_VECTOR_H
#define CRAMION_CORE_SHARED_VECTOR_H

#include <cstddef>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <utility>
#include <vector>

namespace cramion::core {

// std::vector con copia al escribir: copiar uno solo comparte los datos, y la
// copia de verdad se hace la primera vez que alguno de los dos se modifica
// (cualquier acceso no const). Para la geometria de asset::ModelData: las
// variantes de material de RenderSync (.crmat) copian el modelo entero y antes
// cada una guardaba su malla en la RAM; ahora comparten la del original salvo
// que la cambien (el tiling en las UV).
//
// Se usa como un std::vector y se convierte solo a `const std::vector<T>&` (sin
// copiar) y a `std::vector<T>&` (separandose antes si estaba compartido).
// Ojo: en un objeto no const, begin()/operator[]/data() separan; para solo
// leer, por una referencia const (o get()).
template <typename T>
class SharedVector {
public:
    using value_type = T;
    using size_type = std::size_t;
    using iterator = typename std::vector<T>::iterator;
    using const_iterator = typename std::vector<T>::const_iterator;
    using reference = T&;
    using const_reference = const T&;

    SharedVector() = default;
    SharedVector(const std::vector<T>& values) : data_(std::make_shared<std::vector<T>>(values)) {}
    SharedVector(std::vector<T>&& values) : data_(std::make_shared<std::vector<T>>(std::move(values))) {}
    SharedVector(std::initializer_list<T> values) : data_(std::make_shared<std::vector<T>>(values)) {}
    explicit SharedVector(size_type count, const T& value = T())
        : data_(std::make_shared<std::vector<T>>(count, value)) {}
    template <typename It, typename = typename std::iterator_traits<It>::iterator_category>
    SharedVector(It first, It last) : data_(std::make_shared<std::vector<T>>(first, last)) {}

    SharedVector& operator=(const std::vector<T>& values) {
        data_ = std::make_shared<std::vector<T>>(values);
        return *this;
    }
    SharedVector& operator=(std::vector<T>&& values) {
        data_ = std::make_shared<std::vector<T>>(std::move(values));
        return *this;
    }
    SharedVector& operator=(std::initializer_list<T> values) {
        data_ = std::make_shared<std::vector<T>>(values);
        return *this;
    }

    // --- Lectura (sin copiar) ---
    const std::vector<T>& get() const { return data_ ? *data_ : nothing(); }
    operator const std::vector<T>&() const { return get(); }
    size_type size() const { return get().size(); }
    bool empty() const { return get().empty(); }
    size_type capacity() const { return get().capacity(); }
    const T* data() const { return get().data(); }
    const T& operator[](size_type i) const { return get()[i]; }
    const T& at(size_type i) const { return get().at(i); }
    const T& front() const { return get().front(); }
    const T& back() const { return get().back(); }
    const_iterator begin() const { return get().begin(); }
    const_iterator end() const { return get().end(); }
    const_iterator cbegin() const { return get().begin(); }
    const_iterator cend() const { return get().end(); }
    // Comparte los datos con `other` (misma malla, sin copiarla).
    bool sharesWith(const SharedVector& other) const { return data_ != nullptr && data_ == other.data_; }

    // --- Escritura (se separa si estaba compartido) ---
    std::vector<T>& mut() {
        if (!data_) {
            data_ = std::make_shared<std::vector<T>>();
        } else if (data_.use_count() > 1) {
            data_ = std::make_shared<std::vector<T>>(*data_);
        }
        return *data_;
    }
    operator std::vector<T>&() { return mut(); }
    T* data() { return mut().data(); }
    T& operator[](size_type i) { return mut()[i]; }
    T& at(size_type i) { return mut().at(i); }
    T& front() { return mut().front(); }
    T& back() { return mut().back(); }
    iterator begin() { return mut().begin(); }
    iterator end() { return mut().end(); }
    void push_back(const T& value) { mut().push_back(value); }
    void push_back(T&& value) { mut().push_back(std::move(value)); }
    template <typename... Args>
    T& emplace_back(Args&&... args) {
        return mut().emplace_back(std::forward<Args>(args)...);
    }
    void pop_back() { mut().pop_back(); }
    void resize(size_type count) { mut().resize(count); }
    void resize(size_type count, const T& value) { mut().resize(count, value); }
    void reserve(size_type count) { mut().reserve(count); }
    void shrink_to_fit() { mut().shrink_to_fit(); }
    template <typename... Args>
    void assign(Args&&... args) {
        mut().assign(std::forward<Args>(args)...);
    }
    template <typename... Args>
    iterator insert(Args&&... args) {
        return mut().insert(std::forward<Args>(args)...);
    }
    template <typename... Args>
    iterator erase(Args&&... args) {
        return mut().erase(std::forward<Args>(args)...);
    }
    // Vaciar no copia: suelta su parte.
    void clear() { data_.reset(); }
    void swap(SharedVector& other) noexcept { data_.swap(other.data_); }
    void swap(std::vector<T>& other) { mut().swap(other); }
    // Saca el vector (copiandolo solo si estaba compartido).
    std::vector<T> release() {
        std::vector<T> out = std::move(mut());
        data_.reset();
        return out;
    }

    friend bool operator==(const SharedVector& a, const SharedVector& b) { return a.get() == b.get(); }
    friend bool operator==(const SharedVector& a, const std::vector<T>& b) { return a.get() == b; }

private:
    static const std::vector<T>& nothing() {
        static const std::vector<T> none;
        return none;
    }

    std::shared_ptr<std::vector<T>> data_;
};

}  // namespace cramion::core

#endif  // CRAMION_CORE_SHARED_VECTOR_H
