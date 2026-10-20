#ifndef CRAMION_EDITOR_EDITOR_SELECTION_H
#define CRAMION_EDITOR_EDITOR_SELECTION_H

// Seleccion de la Escena: la lista en orden (la ultima es la activa, el
// Inspector y la Jerarquia la muestran asi) mas un indice hash para
// preguntar «¿esta seleccionado?» en O(1). Antes era solo un vector y cada
// fila de la Jerarquia, cada contorno de la Escena y cada icono buscaban en
// el lineal: con miles seleccionados, cuadratico por frame.
//
// La interfaz es la del vector que sustituye (push_back, erase, clear,
// back, recorrido...), asi que el codigo que ya la usaba sigue igual.

#include <CramionCore/Uuid.h>

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <iterator>
#include <unordered_set>
#include <vector>

namespace cramion::editor {

class SelectionSet {
public:
    using const_iterator = std::vector<Uuid>::const_iterator;

    SelectionSet() = default;
    SelectionSet(std::initializer_list<Uuid> list) { assign(list.begin(), list.end()); }
    SelectionSet& operator=(std::initializer_list<Uuid> list) {
        assign(list.begin(), list.end());
        return *this;
    }
    SelectionSet& operator=(const std::vector<Uuid>& list) {
        assign(list.begin(), list.end());
        return *this;
    }
    // Las pestanas de mundo guardan la seleccion como vector (Workspace).
    operator const std::vector<Uuid>&() const { return order_; }
    const std::vector<Uuid>& items() const { return order_; }

    bool contains(const Uuid& uuid) const { return index_.contains(uuid); }
    // Sin duplicados: anadir lo que ya esta no hace nada.
    void push_back(const Uuid& uuid) {
        if (index_.insert(uuid).second) order_.push_back(uuid);
    }
    // Quita `uuid` si estaba (devuelve si estaba). Lineal en la lista, pero
    // solo al deseleccionar uno (Ctrl+clic), no por fila ni por frame.
    bool erase(const Uuid& uuid) {
        if (index_.erase(uuid) == 0) return false;
        order_.erase(std::find_if(order_.begin(), order_.end(), [&](const Uuid& u) { return u == uuid; }));
        return true;
    }
    void clear() {
        order_.clear();
        index_.clear();
    }
    void reserve(std::size_t n) {
        order_.reserve(n);
        index_.reserve(n);
    }

    bool empty() const { return order_.empty(); }
    std::size_t size() const { return order_.size(); }
    const Uuid& back() const { return order_.back(); }
    const Uuid& front() const { return order_.front(); }
    const_iterator begin() const { return order_.begin(); }
    const_iterator end() const { return order_.end(); }

private:
    template <typename It>
    void assign(It first, It last) {
        clear();
        reserve(static_cast<std::size_t>(std::distance(first, last)));
        for (; first != last; ++first) push_back(*first);
    }

    std::vector<Uuid> order_;
    std::unordered_set<Uuid> index_;
};

}  // namespace cramion::editor

#endif  // CRAMION_EDITOR_EDITOR_SELECTION_H
