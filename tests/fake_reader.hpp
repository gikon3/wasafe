#pragma once

#include <string_view>

#include "wasafe/io/reader.hpp"
#include "wasafe/types/logic_vector.hpp"

namespace WaSafe::Fake {

/// Синтетический источник диаграммы: ведёт себя как настоящий парсер формата
/// (двухфазный протокол Builder), но данные генерирует сам. Позволяет проверять
/// связку Reader -> Builder -> Database, не завися ни от одного формата —
/// реализации форматов живут в отдельных проектах.
///
/// Формирует дизайн:
///   top.clk         scalar,  0@0 1@10 0@20 1@30
///   top.data        [7:0],   00000000@0 10100101@20
///   top.temp        real,    1.5@0 2.5@30
///   top.clk_mirror  scalar,  алиас потока clk (одно значение на два имени)
///   top.sub.rst     scalar,  0@0 1@20
class FakeReader final : public Reader {
public:
    FakeReader() = default;
    FakeReader(const FakeReader&) = delete;
    FakeReader(FakeReader&&) = delete;
    ~FakeReader() override = default;

    [[nodiscard]] std::string_view format() const override { return "fake"; }

    void read(Builder& sink) override {
        readHeader(sink);
        emitValues(sink);
    }

    void readHeader(Builder& sink) override {
        sink.setTimeScale({.exponent = static_cast<int>(TimeUnit::NS), .scale = 1});
        sink.beginScope("top", ScopeKind::MODULE);
        clk_ = sink.declareVar("clk", makeScalar());
        data_ = sink.declareVar("data", makeVector(7, 0));
        temp_ = sink.declareVar("temp", makeReal());
        // Алиас: второе имя поверх уже объявленного потока clk.
        [[maybe_unused]] const SignalId mirror = sink.declareVar("clk_mirror", makeScalar(), clk_);
        sink.beginScope("sub", ScopeKind::MODULE);
        rst_ = sink.declareVar("rst", makeScalar());
        sink.endScope();
        sink.endScope();
        sink.headerDone();
    }

    FakeReader& operator=(const FakeReader&) = delete;
    FakeReader& operator=(FakeReader&&) = delete;

private:
    /// Logic-значение заданной ширины из строки бит (MSB слева).
    static LogicVector bits(std::uint32_t width, std::string_view chars) {
        LogicVector v{width};
        v.assignFromChars(chars);
        return v;
    }

    void emitValues(Builder& sink) const {
        const LogicVector lo = bits(1, "0");
        const LogicVector hi = bits(1, "1");

        sink.setTime(0);
        sink.valueChange(clk_, lo);
        sink.valueChange(data_, bits(8, "00000000"));
        sink.valueChange(temp_, 1.5);
        sink.valueChange(rst_, lo);

        sink.setTime(10);
        sink.valueChange(clk_, hi);

        sink.setTime(20);
        sink.valueChange(clk_, lo);
        sink.valueChange(data_, bits(8, "10100101"));
        sink.valueChange(rst_, hi);

        sink.setTime(30);
        sink.valueChange(clk_, hi);
        sink.valueChange(temp_, 2.5);
    }

private:
    SignalId clk_;
    SignalId data_;
    SignalId temp_;
    SignalId rst_;
};

}  // namespace WaSafe::Fake
