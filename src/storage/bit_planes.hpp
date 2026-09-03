#pragma once

#include <cstdint>
#include <vector>

#include "wasafe/types/logic_vector_view.hpp"

namespace WaSafe {

/// Дописать одно logic-значение в конец бит-планов потока (или блока) ширины
/// width. Планы — это КОЛОНКИ всех изменений: i-е занимает в каждом из них слот
/// [i*wordsFor(width), (i+1)*wordsFor(width)), и эта функция добавляет слот.
///
/// Слот заполняется целиком независимо от ширины значения, иначе индексация по
/// i разъедется: разряды сверх width отбрасываются, а незаписанные значением
/// разряды слота читаются как X.
///
/// bval заводится лишь когда он реально нужен: двухзначный поток вдвое дешевле
/// по памяти. Пустой bval означает «все b-биты нулевые».
///
/// @pre aval и bval — планы ОДНОГО набора слотов: bval либо пуст, либо равен
///      aval по длине. Функция сама этот инвариант и поддерживает, но проверить
///      его на входе не может — рассогласованные векторы дадут запись за буфер.
void appendLogicValue(std::uint32_t width, std::vector<LogicVectorView::WordType>& aval,
        std::vector<LogicVectorView::WordType>& bval, LogicVectorView src);

}  // namespace WaSafe
