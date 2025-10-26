/*
 * This file is part of the Pico FIDO distribution (https://github.com/polhenarejos/pico-fido).
 * Copyright (c) 2022 Pol Henarejos.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef __VERSION_H_
#define __VERSION_H_

#define PICO_FIDO_VERSION 0x0606

#define PICO_FIDO_VERSION_MAJOR ((PICO_FIDO_VERSION >> 8) & 0xff)
#define PICO_FIDO_VERSION_MINOR (PICO_FIDO_VERSION & 0xff)

#endif
