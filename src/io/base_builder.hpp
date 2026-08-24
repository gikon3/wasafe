#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <stack>

#include "wasafe/core/time.hpp"
#include "wasafe/io/builder.hpp"
#include "wasafe/model/hierarchy.hpp"

namespace WaSafe {

/// Общая логика построения иерархии и разворачивания композитов. Отличаются
/// наследники лишь тем, КУДА уходят значения (ОЗУ vs нормализованное хранилище):
/// они переопределяют alloc_stream/value_change/finish.
class BaseBuilder : public Builder {
public:
    BaseBuilder();
    void setTimeScale(TimeScale scale) override;
    ScopeId beginScope(std::string_view name, ScopeKind kind) override;
    void endScope() override;
    SignalId declareVar(std::string_view name, Type type, std::optional<SignalId> alias,
            std::span<SignalId> leaves) override;
    void headerDone() override;
    void setTime(TimeStamp time) override;

protected:
    /// Выделить листовой поток заданного вида/ширины. Реализуется наследником.
    virtual SignalId allocStream(ValueKind kind, std::uint32_t width) = 0;

    /// Поток под тип, если он представим ОДНИМ потоком (лист или packed-композит);
    /// для unpacked-композита/void/event — невалидный id. Вид и ширину выбирает
    /// этот метод, а сам факт «поток положен» — singleStreamRepresentable().
    SignalId makeStream(const Type& t);

    /// Поток очередного разворачиваемого элемента: из `leaves` (явная привязка,
    /// см. Builder::declareVar) либо свежевыделенный. Курсор `pos` двигается
    /// только там, где поток действительно выделяется.
    SignalId bindStream(const Type& t, std::span<SignalId> leaves, std::size_t& pos);

    /// Развернуть детей композитного узла: packed — битовые срезы общего потока
    /// `owning` (absolute-смещения в `base`); unpacked — свой поток на лист.
    void buildChildren(NodeId parent, const Type& type, SignalId owning, std::uint32_t base, std::span<SignalId> leaves,
            std::size_t& pos);

protected:
    Hierarchy hierarchy_;
    std::stack<ScopeId> stack_;
    TimeScale scale_{};
    TimeStamp now_ = 0;
};

}  // namespace WaSafe
