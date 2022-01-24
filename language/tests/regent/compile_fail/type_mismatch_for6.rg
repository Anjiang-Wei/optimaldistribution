-- Copyright 2022 Stanford University
--
-- Licensed under the Apache License, Version 2.0 (the "License");
-- you may not use this file except in compliance with the License.
-- You may obtain a copy of the License at
--
--     http://www.apache.org/licenses/LICENSE-2.0
--
-- Unless required by applicable law or agreed to in writing, software
-- distributed under the License is distributed on an "AS IS" BASIS,
-- WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
-- See the License for the specific language governing permissions and
-- limitations under the License.

-- fails-with:
-- type_mismatch_for6.rg:23: iterator for loop expected symbol of type ptr(int32, $r), got ptr(double, $r)
--   for x : ptr(double, r) in r do end
--     ^

import "regent"

task f(r : region(int))
  for x : ptr(double, r) in r do end
end
f()
