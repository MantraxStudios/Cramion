#ifndef CRAMION_CORE_AI_EXPRESSION_H
#define CRAMION_CORE_AI_EXPRESSION_H

// Expresiones de las condiciones de la IA (transiciones "Expresion" de las
// maquinas de estados y el decorador Script Condition de los Behavior
// Trees), sin Lua. Lo que se puede escribir:
//   numeros (5, 2.5, 1e3, 0x10), true / false / nil, textos ("a" o 'a'),
//   variables de la pizarra por su nombre (vida; tambien vars.vida y
//   self.vars.vida, como se escribia en Lua), + - * / // % ^ y .. (unir
//   textos), comparaciones (== ~= != < <= > >=), and / or / not (tambien
//   && || !) y parentesis.
// Sigue las reglas de Lua: `a and b` / `a or b` devuelven uno de los dos
// valores, solo nil y false son falsos, 1 == "1" es falso y comparar con
// < o > cosas de distinto tipo es un error. Un error al evaluar (p. ej. una
// variable que no existe comparada con <) hace la condicion falsa.
// Lo demas de Lua (llamadas a funciones, self.entity, entity.position,
// sm.state...) no se puede leer: la condicion es falsa y validar la maquina
// (o el arbol) lo avisa una vez.
//
// Se lee una vez (ExpressionCache) y cada frame solo se evalua.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace cramion::ai {

struct Value;
struct Variable;

// Un valor al evaluar. Object: entity o vec3 de la pizarra (text = su
// identidad; dos son iguales si son el mismo objeto o el mismo vector).
struct ExprValue {
    enum class Type : std::uint8_t { Nil, Bool, Number, String, Object };
    Type type = Type::Nil;
    double number = 0.0;  // Number; Bool: 0 o 1
    std::string text;     // String / Object

    static ExprValue boolean(bool b);
    static ExprValue num(double n);
    static ExprValue str(std::string s);
    static ExprValue object(std::string id);
    bool truthy() const { return type != Type::Nil && !(type == Type::Bool && number == 0.0); }
};

// El valor de una variable por su nombre (nil si no existe).
using ExprLookup = std::function<ExprValue(const std::string& name)>;

class Expression {
public:
    explicit Expression(std::string_view text);

    // Si no se pudo leer, por que ("llamada a una funcion", "falta ')'"...).
    bool valid() const { return root_ >= 0; }
    const std::string& error() const { return error_; }

    // El resultado (nil y ok = false si no es valida o falla al evaluar).
    ExprValue evaluate(const ExprLookup& lookup, bool* ok = nullptr) const;
    // Verdadero (y sin errores).
    bool test(const ExprLookup& lookup) const;
    // Con las variables de una pizarra (maquina de estados o Behavior Tree).
    bool test(const std::vector<Variable>& vars) const;

    struct Node {
        enum class Kind : std::uint8_t { Constant, Variable, Not, Negate, Binary, And, Or };
        Kind kind = Kind::Constant;
        char op = 0;  // Binary: + - * / f(//) % ^ c(..) = (==) ! (~=) < l(<=) > g(>=)
        int a = -1;
        int b = -1;
        ExprValue value;   // Constant
        std::string name;  // Variable
    };

private:
    friend class ExpressionParser;
    ExprValue eval(int node, const ExprLookup& lookup, bool& ok) const;
    std::vector<Node> nodes_;
    int root_ = -1;
    std::string error_;
};

// Una variable de la pizarra como valor de una expresion: bool, numero
// (int / float), texto; entity (nil si no esta puesta) y vec3 como objetos.
ExprValue exprValueOf(const Value& v);

// La expresion leida de un texto, guardada junto a la condicion: se vuelve a
// leer solo si el texto cambia (el editor la puede cambiar en Play).
class ExpressionCache {
public:
    const Expression& get(const std::string& text) const;

private:
    mutable std::string source_;
    mutable std::shared_ptr<const Expression> expression_;
};

}  // namespace cramion::ai

#endif  // CRAMION_CORE_AI_EXPRESSION_H
