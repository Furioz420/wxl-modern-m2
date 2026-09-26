#pragma once
inline constexpr char kGilneanPreviewLua[] = R"lua(
do
    local owner = _G.WXLGilneanPreview or {}
    _G.WXLGilneanPreview = owner
    local context = _WXL_GILNEAN_CONTEXT
    local trace = _WXL_GILNEAN_TRACE
    local setFront = _WXL_GILNEAN_FRONT
    local optionInfo = _WXL_GILNEAN_OPTION
    local randomize = _WXL_GILNEAN_RANDOMIZE
    local function L(key, fallback)
        local value = _G[key]
        if value and value ~= "" then return value end
        return fallback
    end
    function owner:InstallCamera()
        local api = _G.C_CharacterCreation
        if not api or type(api.GetCameraSettingsZoomed) ~= "function" or self.cameraInstalled then return end
        local original = api.GetCameraSettingsZoomed
        api.GetCameraSettingsZoomed = function(sexID, raceID, ...)
            local settings = original(sexID, raceID, ...)
            local race = context()
            if owner.front and race == 12 and type(settings) == "table" and
                (not raceID or (api.GetSelectedRace and raceID == api.GetSelectedRace())) and
                (not sexID or (api.GetSelectedSex and sexID == api.GetSelectedSex())) then
                -- Positive scene Z lifts the scene in the view, aiming lower.
                -- Copy the endpoint: never modify the shared Worgen camera table.
                return {settings[1], settings[2], settings[3] + (owner.sex == 1 and 0.25 or 0.18)}
            end
            return settings
        end
        self.cameraInstalled = true
    end
    function owner:RefreshCamera()
        local api = _G.C_CharacterCreation
        if api and type(api.ZoomCamera) == "function" then api.ZoomCamera(0, 0.2, true) end
    end
    local function shown(frame)
        if not frame or not frame.IsShown then return false end
        local value = frame:IsShown()
        return value == true or value == 1
    end
    local function modelCall(model, method, ...)
        local fn = model and model[method]
        if type(fn) ~= "function" then return false end
        return pcall(fn, model, ...)
    end
    function owner:Report(message)
        if self.lastReport ~= message then
            self.lastReport = message
            trace(message)
        end
    end
    function owner:Hide()
        local wasFront = self.front
        setFront(0)
        self.front = false
        if wasFront then self:RefreshCamera() end
        self.zoom = 0
        self:EditorVisibility()
        if self.model then
            self.model:Hide()
            self.model:WXLClearGilnean()
            if self.loaded then modelCall(self.model, "ClearModel") end
        end
        if self.controls then self.controls:Hide() end
        if self.swap then self.swap:Hide() end
        self.key, self.loaded, self.ready, self.wait, self.bodyCamera = nil, false, false, 0, false
    end
    function owner:Layout()
        self.model:ClearAllPoints()
        local baseHeight = self.front and 450 or 290
        local height = math.max(baseHeight, CharacterCreate:GetHeight())
        self.viewportRatio = height / baseHeight
        self.model:SetSize(self.front and 600 or 230, height)
        self.model:SetPoint("CENTER", CharacterCreate, "BOTTOM", self.front and 0 or -300, self.front and 375 or 330)
        self.controls:ClearAllPoints()
        local panel = CharacterCreate.CustomizationFrame or CharacterCreate
        self.controls:SetPoint("TOP", panel, "TOP", 0, -145)
        self.swap:ClearAllPoints()
        -- Name entry now lives above the model; form switching stays in the footer.
        self.swap:SetPoint("BOTTOM", CharacterCreate, "BOTTOM", 0, 4)
        self:EditorVisibility()
        local raceName = self.front and
            (self.primaryName or L("WXL_CC_WORGEN", "Worgen")) or
            (self.alternateName or L("WXL_CC_GILNEAN", "Gilnean"))
        self.swap:SetText(string.format(
            L("WXL_CC_SHOW_IN_CENTER", "Show %s in center"), raceName))
    end
    function owner:EditorVisibility()
        local panel = CharacterCreate and CharacterCreate.CustomizationFrame
        local bridge = _G.WXLModernM2CustomizationBridge
        if self.front then
            if self.controls then self.controls:Show() end
            if bridge and bridge.HidePalette then bridge:HidePalette() end
            if panel and panel.customizationButtonFramePool then
                for row in panel.customizationButtonFramePool:EnumerateActive() do row:Hide() end
            end
            if bridge and panel then bridge:SetupTabs(panel) end
            if panel and panel.WXLPage then panel.WXLPage:Hide() end
            if panel and panel.RandomizeCustomizationButton then panel.RandomizeCustomizationButton:Hide() end
            self.editorHidden = true
        else
            if self.controls then self.controls:Hide() end
            if self.editorHidden and bridge and bridge.Layout and panel then bridge:Layout(panel) end
            self.editorHidden = false
        end
    end
    function owner:RefreshOptions()
        local visibleRows = 0
        local indices = {}
        local bridge = _G.WXLModernM2CustomizationBridge
        local category = bridge and bridge.category or "body"
        local panel = CharacterCreate and CharacterCreate.CustomizationFrame
        local pageSize = bridge and panel and bridge:PageSize(panel) or 8
        for index = 0, 63 do
            local name = optionInfo(self.sex, index)
            if name and name ~= "" then
                local text = string.lower(name)
                local body = false
                for _, word in ipairs({"skin", "body", "primary", "secondary", "scale", "tail", "wing", "armor", "armour", "chest", "leg", "foot", "feet", "underwear"}) do
                    if string.find(text, word, 1, true) then body = true end
                end
                if category == (body and "body" or "head") then indices[#indices + 1] = index end
            end
        end
        local pages = math.max(1, math.ceil(#indices / pageSize))
        self.page = math.min(self.page or 1, pages)
        if self.previousPage then
            if pages > 1 then self.pagination:Show(); self.previousPage:Show(); self.nextPage:Show()
            else self.pagination:Hide(); self.previousPage:Hide(); self.nextPage:Hide() end
            self.pageLabel:SetText(pages > 1 and (self.page .. " / " .. pages) or "")
        end
        for i, row in ipairs(self.rows) do
            row.optionIndex = i <= pageSize and indices[(self.page - 1) * pageSize + i] or nil
            local name, choice, selected, count, swatch
            if row.optionIndex then name, choice, selected, count, swatch = optionInfo(self.sex, row.optionIndex) end
            if name then
                visibleRows = i
                local displayName = bridge and bridge.LocalizeOption and
                    bridge:LocalizeOption(name) or name
                local value = choice and choice ~= "" and
                    (bridge and bridge.LocalizeChoice and bridge:LocalizeChoice(choice) or choice) or
                    (selected .. "/" .. count)
                if bridge and bridge.SetRowText then
                    bridge:StyleRow(row)
                    bridge:SetRowText(row, displayName, value)
                    row:ClearAllPoints()
                    row:SetPoint("TOP", self.controls, "TOP", 82, -30 - (i - 1) * 36)
                else row.CustomizationName:SetText(displayName .. ": " .. value) end
                local bridge = _G.WXLModernM2CustomizationBridge
                if bridge and bridge.SetSwatch then
                    bridge.swatchOwner = row
                    row.WXLSwatch = bridge:SetSwatch(row.WXLSwatch, swatch, swatch and swatch ~= 0)
                    bridge.swatchOwner = nil
                    if row.WXLSwatch then
                        row.WXLSwatch:ClearAllPoints()
                        row.WXLSwatch:SetPoint("CENTER", row, "CENTER", 0, 0)
                        if swatch and swatch ~= 0 then row.CustomizationName:SetText("") end
                    end
                end
                row:Show()
            else row:Hide() end
        end
        if self.randomize then
            -- Keep pagination in the editor footer, clear of the model toolbar.
            self.pagination:ClearAllPoints()
            self.pagination:SetPoint("TOP", self.controls, "TOP", 24, -30 - visibleRows * 36)
            self.randomize:ClearAllPoints()
            self.randomize:SetPoint("TOP", panel or self.controls, "TOP", -30, -78)
            if bridge and bridge.StyleDice then bridge:StyleDice(self.randomize) end
        end
    end
    function owner:Ensure(root)
        if self.model then return end
        local model = CreateFrame("Model", "WXLGilneanPreviewModel", root)
        self.model = model
        model:SetFrameStrata("DIALOG")
        model.WXLUpdateGilnean = _WXL_GILNEAN_UPDATE
        model.WXLClearGilnean = _WXL_GILNEAN_CLEAR
        model:EnableMouse(false)
        model:EnableMouseWheel(false)
        model:SetScript("OnMouseDown", function(self, button)
            if button == "LeftButton" and owner.ready then
                local api = _G.C_CharacterCreation
                local getter = api and api.GetCharacterCreateFacing or _G.GetCharacterCreateFacing
                local setter = api and api.SetCharacterCreateFacing or _G.SetCharacterCreateFacing
                if type(getter) == "function" and type(setter) == "function" then
                    local ok, angle = pcall(getter)
                    if ok and type(angle) == "number" then
                        self.dragX, self.dragFacing, self.rotate = GetCursorPosition(), angle, setter
                    end
                end
            end
        end)
        model:SetScript("OnMouseUp", function(self) self.dragX = nil end)
        model:SetScript("OnHide", function(self) self.dragX = nil end)
        model:SetScript("OnUpdate", function(self, elapsed)
            if owner.loaded then

                local ok, ready, fixedCamera = pcall(self.WXLUpdateGilnean, self, owner.sex, owner.front and (owner.zoom or 0) or 0, owner.viewportRatio or 1)
                if ok and ready and fixedCamera then
                    local api = _G.C_CharacterCreation
                    local getter = api and api.GetCharacterCreateFacing or _G.GetCharacterCreateFacing
                    if type(getter) == "function" then
                        local got, degrees = pcall(getter)
                        if got and type(degrees) == "number" then
                            modelCall(self, "SetFacing", degrees * math.pi / 180)
                        end
                    end
                end
                owner.ready = ok and ready and true or false
                self:SetAlpha(0) -- scene actor renders through the primary creation camera
                owner.wait = owner.ready and 0 or (owner.wait or 0) + (elapsed or 0)
                local label = (owner.alternateName or "Gilnean") .. " Form"
                local text = owner.ready and label or
                    (owner.wait < 10 and label .. " - Loading..." or label .. " unavailable")
                if text ~= owner.titleText then owner.titleText = text; owner.title:SetText(text) end
                if owner.ready then owner.swap:Enable() else owner.swap:Disable() end
                if not ok then owner:Report("model update error: " .. tostring(ready)) end
            end
            if self.dragX then pcall(self.rotate, self.dragFacing + (GetCursorPosition() - self.dragX) * 0.6) end
        end)
        local controls = CreateFrame("Frame", "WXLGilneanCustomization", root)
        self.controls = controls
        controls:SetSize(270, 315)
        controls:SetFrameStrata("FULLSCREEN_DIALOG")
        self.title = controls:CreateFontString(nil, "OVERLAY", "GameFontNormalLarge")
        self.title:SetPoint("TOP", 0, 0)
        self.rows = {}
        for i = 1, 8 do
            local index = i - 1
            -- Share the same atlas, frame and arrow hover/press states as the stock editor.
            local row = CreateFrame("Frame", "WXLGilneanOption" .. i, controls,
                "CharacterCreateCustomizationButtonFrameTemplate")
            row:SetSize(200, 30)
            row:SetPoint("TOP", 0, -30 - index * 36)
            row.Background:SetTexture("Interface\\Glues\\CharacterCreate\\UI-CharacterCreatePatchwerk")
            row.Background:SetTexCoord(0.00195313, 0.41992188, 0.60351563, 0.68652344)
            row.CustomizationName:ClearAllPoints()
            row.CustomizationName:SetPoint("CENTER", row, "CENTER", 0, 0)
            for _, direction in ipairs({-1, 1}) do
                local delta = direction
                local button = delta < 0 and row.PrevButton or row.NextButton
                -- Replace only the click dispatch. Template appearance and interaction remain.
                button:SetScript("OnClick", function()
                    if not owner.front or not owner.ready then return end
                    optionInfo(owner.sex, row.optionIndex, delta)
                    local bridge = _G.WXLModernM2CustomizationBridge
                    if bridge and bridge.ComposeStock then bridge:ComposeStock(1) end
                    if PlaySound then PlaySound("gsCharacterCreationLook") end
                    owner:RefreshOptions()
                end)
            end
            self.rows[i] = row
        end
        self.randomize = CreateFrame("Button", "WXLGilneanRandomize", controls, "GlueDark_ButtonTemplate")
        self.randomize:SetSize(160, 40)
        self.randomize:SetText(RANDOMIZE or "Randomize")
        self.randomize:SetScript("OnClick", function()
            if not owner.front or not owner.ready or not randomize then return end
            local bridge = _G.WXLModernM2CustomizationBridge
            local function apply()
                if not owner.front or not owner.ready then return end
                if randomize() then
                    if bridge and bridge.ComposeStock then bridge:ComposeStock(1) end
                    owner:RefreshOptions()
                    if PlaySound then PlaySound("gsCharacterCreationLook") end
                end
            end
            if bridge and bridge.QueueCustomizationWork then bridge:QueueCustomizationWork(apply)
            else apply() end
        end)
        self.randomize:Show()
        self.pagination = CreateFrame("Frame", "WXLAlternateFormPagination", controls,
            "CharacterCreateCustomizationButtonFrameTemplate")
        self.pagination:SetSize(140, 30)
        self.pagination.Background:Hide()
        self.previousPage = self.pagination.PrevButton
        self.nextPage = self.pagination.NextButton
        self.pageLabel = self.pagination.CustomizationName
        self.pageLabel:ClearAllPoints()
        self.pageLabel:SetPoint("CENTER", self.pagination, "CENTER", 0, 0)
        for _, entry in ipairs({{self.previousPage, -1}, {self.nextPage, 1}}) do
            local button, direction = entry[1], entry[2]
            button:SetScript("OnClick", function()
                owner.page = math.max(1, (owner.page or 1) + direction)
                owner:RefreshOptions()
            end)
        end
        -- Same red artwork, without the dark template's extra glow/edit-box layers.
        self.swap = CreateFrame("Button", "WXLSwapWorgenForm", root, "GlueModernButtonTemplate")
        self.swap:SetSize(220, 32)
        self.swap:SetFrameStrata("FULLSCREEN_DIALOG")
        self.swap:SetScript("OnClick", function()
            if owner.ready and setFront(owner.front and 0 or 1) then
                owner.front = not owner.front
                owner:RefreshCamera()
                owner.zoom = 0
                owner:Layout()
                trace((owner.front and owner.alternateName or owner.primaryName) .. " centered")
            end
        end)
        self:Layout()
        self:Report("model, linked option rows and swap button created")
    end
    function owner:Refresh()
        self:InstallCamera()
        local root = _G.CharacterCreate
        if not shown(root) then
            self:Hide(); self:Report("creation frame hidden"); return
        end
        -- The native component supplies race/sex. Do not call GetSelectedSex through a possibly
        -- uninitialized stock creation singleton, or depend on another bridge's bootstrap state.
        local race, sex, path, file = context()
        if race ~= 12 and race ~= 17 and race ~= 30 then self:Hide(); self:Report("waiting for alternate-form component, race=" .. tostring(race)); return end
        local panel = root.CustomizationFrame or _G.CharacterCreateCustomizationFrame
        local open = panel and (panel.show == true or panel.show == 1)
        if not open then self:Hide(); self:Report("Worgen customization closed"); return end
        self:Ensure(root)
        local key = tostring(race) .. ":" .. tostring(sex) .. ":" .. tostring(file)
        if self.key ~= key then
            self:Hide()
            self.sex, self.key, self.race, self.page = sex, key, race, 1
            self.primaryName = race == 12 and
                L("WXL_CC_WORGEN", "Worgen") or L("WXL_CC_VISAGE", "Visage")
            self.alternateName = race == 12 and
                L("WXL_CC_GILNEAN", "Gilnean") or L("WXL_CC_DRACTHYR", "Dracthyr")
            self:Layout()
            self.model:SetAlpha(0)
            self.loaded = path and path ~= "" and modelCall(self.model, "SetModel", path) or false
            modelCall(self.model, "SetModelScale", 0.72)
            modelCall(self.model, "SetPosition", 0, 0, 0)
            modelCall(self.model, "SetFacing", 0)
            modelCall(self.model, "SetSequence", 0)
            modelCall(self.model, "SetLight", 1, 0, 0, -0.707, -0.707, 0.7, 1, 1, 1, 0.8, 1, 1, 0.8)
            self.titleText = self.alternateName .. (self.loaded and " Form - Loading..." or " preview unavailable")
            self.title:SetText(self.titleText)
            self:Report("Worgen active sex=" .. tostring(sex) .. " model=" .. tostring(file) .. " loaded=" .. tostring(self.loaded))
        end
        self:RefreshOptions()
        self.model:Show(); self:EditorVisibility(); self.swap:Show()
        if self.ready then self.swap:Enable() else self.swap:Disable() end
    end
    if not owner.driver then
        owner.driver = CreateFrame("Frame", "WXLGilneanPreviewDriver", _G.GlueParent)
        owner.driver:SetSize(1, 1)
        owner.driver:SetPoint("TOPLEFT", _G.GlueParent, "TOPLEFT", 0, 0)
        owner.driver:SetScript("OnUpdate", function(_, elapsed)
            owner.elapsed = (owner.elapsed or 0) + (elapsed or 0)
            if owner.elapsed < 0.1 then return end
            owner.elapsed = 0
            local ok, err = pcall(owner.Refresh, owner)
            if not ok then
                pcall(owner.Hide, owner)
                owner:Report("UI error: " .. tostring(err))
            end
        end)
        owner.driver:Show()
    end
    trace("independent preview controller initialized")
end

)lua";
